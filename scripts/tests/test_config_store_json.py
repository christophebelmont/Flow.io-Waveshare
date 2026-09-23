"""Compile production ConfigStore export methods and round-trip their JSON."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
ARDUINO_JSON = ROOT / ".pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src"

PREAMBLE = r'''
#include <ArduinoJson.h>
#include "Core/ConfigTypes.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
class ConfigStore {
public:
    ConfigMeta* _meta;
    uint16_t _metaCount;
    void toJson(char*, size_t) const;
    bool toJsonModule(const char*, char*, size_t, bool*, bool = true) const;
};
'''

PROGRAM = r'''
int main() {
    char driver[] = R"({"kind":0,"outputs":[0]})";
    char label[] = "Pompe \"été\" \\ local\n\r\t\b\f\x01";
    char secret[] = "secret\"\\";
    char empty[] = "";
    bool enabled = true;
    ConfigMeta meta[5]{};
    const char* names[] = {"driver", "label", "pass", "empty", "enabled"};
    void* values[] = {driver, label, secret, empty, &enabled};
    for (unsigned i = 0; i < 5; ++i) {
        meta[i].module = "pdm/pd0";
        meta[i].name = names[i];
        meta[i].type = i == 4 ? ConfigType::Bool : ConfigType::CharArray;
        meta[i].valuePtr = values[i];
    }
    ConfigStore store{meta, 5};
    char output[1024];
    for (int mode = 0; mode < 3; ++mode) {
        bool truncated = true;
        if (mode == 0) store.toJson(output, sizeof(output));
        else {
            assert(store.toJsonModule("pdm/pd0", output, sizeof(output), &truncated, mode == 1));
            assert(!truncated);
        }
        StaticJsonDocument<2048> parsed;
        assert(!deserializeJson(parsed, output));
        assert(strcmp(parsed["driver"], driver) == 0);
        assert(strcmp(parsed["label"], label) == 0);
        assert(strcmp(parsed["empty"], "") == 0);
        assert(strcmp(parsed["pass"], mode == 1 ? "***" : secret) == 0);
        assert(parsed["enabled"].as<bool>());
        StaticJsonDocument<256> nested;
        assert(!deserializeJson(nested, parsed["driver"].as<const char*>()));
        assert(nested["outputs"][0].as<int>() == 0);
    }

    // Every buffer boundary, including expansion by escaped quotes, stays bounded.
    bool truncated = false;
    store.toJsonModule("pdm/pd0", output, sizeof(output), &truncated);
    const std::string expected(output);
    for (size_t capacity = 1; capacity <= expected.size() + 1; ++capacity) {
        std::vector<char> guarded(capacity + 2, '#');
        store.toJsonModule("pdm/pd0", guarded.data() + 1, capacity, &truncated);
        assert(guarded.front() == '#' && guarded.back() == '#');
        assert(memchr(guarded.data() + 1, '\0', capacity));
        assert(truncated == (capacity <= expected.size()));
        if (!truncated) assert(expected == guarded.data() + 1);
        store.toJson(guarded.data() + 1, capacity);
        assert(guarded.front() == '#' && guarded.back() == '#');
        assert(memchr(guarded.data() + 1, '\0', capacity));
    }
}
'''


class ConfigStoreJsonTest(unittest.TestCase):
    def test_exports(self):
        source = (ROOT / "src/Core/ConfigStore.cpp").read_text()
        # Exercise the actual methods without NVS/FreeRTOS dependencies.
        start = source.index("static bool isMaskedKey(")
        end = source.index("uint8_t ConfigStore::listModules(", start)
        with tempfile.TemporaryDirectory(prefix="flow-config-json-") as directory:
            work = Path(directory)
            cpp = work / "main.cpp"
            cpp.write_text(PREAMBLE + source[start:end] + PROGRAM)
            binary = work / "test"
            subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src"), "-I", str(ARDUINO_JSON),
                "-I", str(ROOT / "include"),
                str(cpp), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()

"""Exercise the production JSON allocator with ArduinoJson and a fake ESP heap.

Run after PlatformIO has installed ArduinoJson:
    python3 -m unittest scripts.tests.test_spiram_json_document
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
ARDUINO_JSON = ROOT / ".pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src"

HEAP_HEADER = r"""
#pragma once
#include <cassert>
#include <cstdlib>
#include <cstddef>
constexpr unsigned MALLOC_CAP_SPIRAM = 1;
constexpr unsigned MALLOC_CAP_8BIT = 2;
inline bool failAllocation = false;
inline unsigned liveAllocations = 0;
inline void* heap_caps_malloc(size_t bytes, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (failAllocation) return nullptr;
    void* p = std::malloc(bytes);
    if (p) ++liveAllocations;
    return p;
}
inline void heap_caps_free(void* p) {
    if (p) { assert(liveAllocations > 0); --liveAllocations; }
    std::free(p);
}
inline void* heap_caps_realloc(void* p, size_t bytes, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (failAllocation) return nullptr;
    assert(p && bytes);
    return std::realloc(p, bytes);
}
"""

TEST_PROGRAM = r"""
#include "Core/SpiRamJsonDocument.h"
#include <cstring>

int main() {
    // Overlapping requests must not invalidate each other's views.
    {
        SpiRamJsonDocument first(1024);
        assert(!deserializeJson(first, R"({"args":{"id":42,"value":"first"}})"));
        JsonObjectConst view = first["args"];
        {
            SpiRamJsonDocument second(1024);
            assert(!deserializeJson(second, R"({"id":7,"value":"second"})"));
            assert(liveAllocations == 2);
            assert(view["id"].as<int>() == 42);
            assert(std::strcmp(view["value"], "first") == 0);
        }
        assert(liveAllocations == 1);
        assert(view["id"].as<int>() == 42);
    }
    assert(liveAllocations == 0);

    // No fallback allocation when PSRAM is unavailable.
    failAllocation = true;
    {
        SpiRamJsonDocument unavailable(1024);
        assert(unavailable.capacity() == 0);
        assert(deserializeJson(unavailable, R"({"id":42})") ==
               DeserializationError::NoMemory);
        assert(liveAllocations == 0);
    }
    failAllocation = false;

    // Malformed/oversized requests fail; subsequent requests still work.
    {
        SpiRamJsonDocument doc(1024);
        assert(deserializeJson(doc, "{invalid") != DeserializationError::Ok);
        assert(!deserializeJson(doc, R"({"id":42})"));
        doc.shrinkToFit();
        assert(doc["id"].as<int>() == 42);
        char output[32]{};
        serializeJson(doc, output, sizeof(output));
        assert(std::strcmp(output, R"({"id":42})") == 0);
        SpiRamJsonDocument small(8);
        assert(deserializeJson(small, R"({"id":42})") == DeserializationError::NoMemory);
    }
    assert(liveAllocations == 0);
}
"""


class SpiRamJsonDocumentTest(unittest.TestCase):
    def test_lifetime_allocation_failure_and_parsing(self):
        self.assertIsNotNone(shutil.which("c++"), "A native C++ compiler is required")
        self.assertTrue(ARDUINO_JSON.is_dir(), "Run PlatformIO to install ArduinoJson first")
        with tempfile.TemporaryDirectory(prefix="flow-json-test-") as directory:
            work = Path(directory)
            (work / "esp_heap_caps.h").write_text(HEAP_HEADER)
            (work / "main.cpp").write_text(TEST_PROGRAM)
            executable = work / "test"
            subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(work), "-I", str(ROOT / "src"),
                "-I", str(ARDUINO_JSON), str(work / "main.cpp"),
                "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()

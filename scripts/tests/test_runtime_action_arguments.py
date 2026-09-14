"""Exercise the production HTTP command argument builder with ArduinoJson."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
ARDUINO_JSON = ROOT / ".pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src"

TEST_PROGRAM = r'''
#include "Modules/Network/WebInterfaceModule/RuntimeActionArguments.h"
#include "Core/Generated/RuntimeUiManifest_Generated.h"
#include "Modules/PoolLogicModule/ManualDeviceCommand.h"
#include "Profiles/Waveshare/PoolDeviceHaCommand.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    const auto* action = findRuntimeUiActionManifestItem(2305, "set_device");
    assert(action && action->inputType == RuntimeUiActionInputType::Bool);
    assert(std::strcmp(action->command, "poollogic.device.write") == 0);

    // The popup must reach the exact filtration command used by the card,
    // including when the configured pump is outside the dashboard's slots.
    const auto* filtrationTile = findRuntimeUiActionManifestItem(2301, "set");
    assert(filtrationTile);
    const PoolManualDeviceSlots defaults{0, 1, 2, 3, 7, 5};
    assert(std::strcmp(poolManualDeviceWriteCommand(0, defaults), filtrationTile->command) == 0);
    const PoolManualDeviceSlots remapped{8, 9, 10, 11, 12, 13};
    assert(std::strcmp(poolManualDeviceWriteCommand(8, remapped), filtrationTile->command) == 0);
    assert(std::strcmp(poolManualDeviceWriteCommand(0, remapped), "pooldevice.write") == 0);
    assert(std::strcmp(poolManualDeviceWriteCommand(15, remapped), "pooldevice.write") == 0);
    assert(std::strcmp(poolManualDeviceWriteCommand(9, remapped), "poollogic.ph_pump.write") == 0);
    assert(std::strcmp(poolManualDeviceWriteCommand(10, remapped), "poollogic.dis_pump.write") == 0);
    assert(std::strcmp(poolManualDeviceWriteCommand(11, remapped), "poollogic.robot.write") == 0);
    assert(std::strcmp(poolManualDeviceWriteCommand(12, remapped), "poollogic.heater.write") == 0);
    assert(std::strcmp(poolManualDeviceWriteCommand(13, remapped), "poollogic.chlorine_generator.write") == 0);
    assert(std::strcmp(poolManualDeviceWriteCommand(6, remapped), "poollogic.lights.write") == 0);

    // Every command exposed by an equipment card must match the popup's
    // resolved command, for both the default and reconfigured role bindings.
    for (const auto& roles : {defaults, remapped}) {
        const uint8_t slots[] = {roles.filtration, roles.phPump, roles.disinfectionPump, roles.robot};
        for (unsigned index = 0; index < 4; ++index) {
            const auto* tile = findRuntimeUiActionManifestItem(2301 + index, "set");
            assert(tile && tile->inputType == RuntimeUiActionInputType::Bool);
            assert(std::strcmp(poolManualDeviceWriteCommand(slots[index], roles), tile->command) == 0);
        }
    }

    // Decode actual HA discovery payloads as HA does, then verify that the
    // MQTT command reaches the same dispatcher and slot as the web popup.
    for (unsigned slot = 0; slot < 16; ++slot) {
        for (const bool on : {false, true}) {
            char payload[160]{};
            assert(formatPoolDeviceHaWritePayload(payload, sizeof(payload), slot, on));
            char discovery[192]{};
            std::snprintf(discovery, sizeof(discovery), "{\"pl\":\"%s\"}", payload);
            StaticJsonDocument<512> config;
            assert(!deserializeJson(config, static_cast<const char*>(discovery)));
            StaticJsonDocument<512> command;
            assert(!deserializeJson(command, config["pl"].as<const char*>()));
            assert(std::strcmp(command["cmd"], action->command) == 0);
            assert(command["args"]["slot"].is<uint8_t>() && command["args"]["slot"].as<uint8_t>() == slot);
            assert(command["args"]["value"].is<bool>() && command["args"]["value"].as<bool>() == on);
        }
    }
    char shortPayload[8]{};
    assert(!formatPoolDeviceHaWritePayload(shortPayload, sizeof(shortPayload), 15, true));

    // Exercise both directions and non-contiguous equipment slots. The output
    // must remain valid when the same HTTP arguments buffer is reused.
    for (const bool on : {false, true}) {
        for (const unsigned slot : {0U, 8U, 15U}) {
            char args[96]{};
            std::snprintf(args, sizeof(args), "{\"%s\":%s}", action->inputName, on ? "true" : "false");
            assert(appendRuntimeActionTarget(args, sizeof(args), action->targetName, slot));
            StaticJsonDocument<192> received;
            assert(!deserializeJson(received, static_cast<const char*>(args)));
            JsonObjectConst view = received.as<JsonObjectConst>();
            assert(view.size() == 2);
            assert(view["value"].is<bool>() && view["value"].as<bool>() == on);
            assert(view["slot"].is<uint8_t>() && view["slot"].as<uint8_t>() == slot);
        }
    }

    // Reject an insufficient output buffer without sending truncated arguments.
    char small[16] = "{\"value\":true}";
    assert(!appendRuntimeActionTarget(small, sizeof(small), "slot", 15));
    assert(std::strcmp(small, "{\"value\":true}") == 0);

    char malformed[96] = "{invalid";
    assert(!appendRuntimeActionTarget(malformed, sizeof(malformed), "slot", 15));
    char array[96] = "[]";
    assert(!appendRuntimeActionTarget(array, sizeof(array), "slot", 15));

    // Preserve typed values for other targeted runtime actions as well.
    char number[96] = "{\"count\":4294967295}";
    assert(appendRuntimeActionTarget(number, sizeof(number), "target_id", UINT32_MAX));
    StaticJsonDocument<192> received;
    assert(!deserializeJson(received, static_cast<const char*>(number)));
    assert(received["count"].as<uint32_t>() == UINT32_MAX);
    assert(received["target_id"].as<uint32_t>() == UINT32_MAX);
}
'''


class RuntimeActionArgumentsTest(unittest.TestCase):
    def test_targeted_command_arguments_are_valid_and_typed(self):
        self.assertIsNotNone(shutil.which("c++"), "A native C++ compiler is required")
        self.assertTrue(ARDUINO_JSON.is_dir(), "Run PlatformIO to install ArduinoJson first")
        with tempfile.TemporaryDirectory(prefix="flow-runtime-action-test-") as directory:
            work = Path(directory)
            source = work / "main.cpp"
            source.write_text(TEST_PROGRAM)
            executable = work / "test"
            subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src"), "-I", str(ARDUINO_JSON),
                str(source), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()

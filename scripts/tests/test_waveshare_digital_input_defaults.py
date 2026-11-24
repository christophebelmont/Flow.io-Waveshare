#!/usr/bin/env python3
"""Regression checks for Waveshare first-boot digital input defaults.

These are source-level checks, not an ESP32 hardware or NVS integration test.
The profile seeds ConfigStore variables before NVS is loaded. Existing NVS
entries must be authoritative and absent entries keep their profile defaults.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def read(relative_path):
    return (ROOT / relative_path).read_text(encoding="utf-8")


class WaveshareDigitalInputDefaultsTests(unittest.TestCase):
    def test_domain_role_polarities_and_debounce(self):
        layout = read("src/Profiles/Waveshare/WaveshareIoLayout.h")
        defaults = (
            "{PoolIds::SensorWaterMeter, PortGpio5Input, false, IO_PULL_NONE, IO_DIGITAL_INPUT_COUNTER, IO_EDGE_RISING, 1000U}",
            "{PoolIds::SensorPoolLevel, PortGpio6Input, true, IO_PULL_NONE, IO_DIGITAL_INPUT_STATE, IO_EDGE_RISING, 0U}",
            "{PoolIds::SensorChlorineLevel, PortGpio7Input, true, IO_PULL_NONE, IO_DIGITAL_INPUT_STATE, IO_EDGE_RISING, 0U}",
            "{PoolIds::SensorPhLevel, PortGpio8Input, true, IO_PULL_NONE, IO_DIGITAL_INPUT_STATE, IO_EDGE_RISING, 0U}",
            "{PoolIds::SensorPir, PortGpio11Input, true, IO_PULL_UP, IO_DIGITAL_INPUT_STATE, IO_EDGE_RISING, 0U}",
        )
        for row in defaults:
            with self.subTest(row=row):
                self.assertIn(row, layout)

    def test_gpio9_flow_defaults_and_spare_gpio10(self):
        assembly = read("src/Profiles/Waveshare/WaveshareIoAssembly.cpp")
        block = assembly.split("if (i == PoolInputSlots::FlowMeter) {", 1)[1].split("\n        }", 1)[0]
        for setting in (
            "def.activeHigh = true;",
            "def.pullMode = IO_PULL_NONE;",
            "def.mode = IO_DIGITAL_INPUT_PULSES_PER_MINUTE;",
            "def.edgeMode = IO_EDGE_RISING;",
            "def.counterDebounceUs = 100U;",
            "def.flowUnit = IO_FLOW_LITERS_PER_MINUTE;",
            "def.c0 = 23.0f;",
            "def.precision = 0;",
        ):
            with self.subTest(setting=setting):
                self.assertIn(setting, block)
        self.assertIn('case PoolInputSlots::Spare: return "GPIO10";', assembly)
        self.assertIn("digitalInputPortFromOrdinal((uint8_t)(i + 1U))", assembly)

    def test_initial_values_reach_registered_config(self):
        definition = read("src/Modules/IOModule/IOModuleTypes.h")
        implementation = read("src/Modules/IOModule/IOModule.cpp")
        descriptors = read("src/Modules/IOModule/IOConfigDescriptorStorage.h")
        for field in ("flowUnit", "c0", "precision"):
            self.assertIn(f"digitalInCfg_[logicalIdx].{field} = def.{field};", implementation)
            self.assertIn(f"digitalInputCfg[slot].{field}", descriptors)
        self.assertIn("uint8_t flowUnit = IO_FLOW_LITERS_PER_MINUTE;", definition)
        self.assertIn("float c0 = 1.0f;", definition)
        self.assertIn("int32_t precision = 0;", definition)

    def test_persisted_values_override_defaults_at_boot(self):
        manager = read("src/Core/ModuleManager.cpp")
        config = read("src/Core/ConfigStore.cpp")
        self.assertLess(
            manager.index("ordered[i]->init(cfg, services);"),
            manager.index("cfg.loadPersistent();"),
        )
        self.assertLess(
            manager.index("cfg.loadPersistent();"),
            manager.index("ordered[i]->onConfigLoaded(cfg, services);"),
        )
        # Missing keys keep the in-memory default; existing keys overwrite it.
        for getter in ("getBool", "getUChar", "getInt", "getFloat"):
            self.assertIn(f"_prefs->{getter}(", config)
        self.assertIn("_prefs->getFloat(m.nvsKey, *(float*)m.valuePtr)", config)
        self.assertIn("_prefs->getUChar(m.nvsKey, *(uint8_t*)m.valuePtr)", config)


if __name__ == "__main__":
    unittest.main()

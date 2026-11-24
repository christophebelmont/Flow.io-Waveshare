#!/usr/bin/env python3
"""Source-level regression checks for web dashboard / TFT S3 sensor slots.

Run: python3 -m unittest scripts/tests/test_web_dashboard_tft_sensor_slots.py
These checks do not replace a PlatformIO build or browser/device tests.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
WEB = (ROOT / "src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp").read_text(encoding="utf-8")
UI = (ROOT / "data/webinterface/app.js").read_text(encoding="utf-8")
TFT_H = (ROOT / "src/Modules/TFTModuleS3/TFTModuleS3.h").read_text(encoding="utf-8")
TFT_CPP = (ROOT / "src/Modules/TFTModuleS3/TFTModuleS3.cpp").read_text(encoding="utf-8")


class DashboardSensorSlotsTest(unittest.TestCase):
    def test_web_and_tft_both_support_ten_sensor_slots(self):
        self.assertRegex(TFT_H, r"DashboardSlotCount\\s*=\\s*10\\s*;")
        self.assertIn("kWaveshareDashboardSensorSlotCount = 10U;", WEB)
        self.assertIn("kWaveshareDashboardDefaultEnabledSlots = 8U;", WEB)
        self.assertIn("slotCfg.enabled = (i < 8U)", TFT_CPP)

    def test_alarm_slots_stay_at_eight(self):
        self.assertIn("kWaveshareAlarmDashboardSlotCount = 8U;", WEB)
        self.assertIn("AlarmDashboardSlotCount = 8;", TFT_H)
        alarm = WEB.split("void sendWaveshareAlarmDashboardSlotsResponse_(", 1)[1].split(
            "bool parseRuntimeUiIdsCsv_", 1
        )[0]
        self.assertIn("i < kWaveshareAlarmDashboardSlotCount", alarm)

    def test_new_sensor_slots_default_opt_in(self):
        self.assertIn("out.enabled = slot < kWaveshareDashboardDefaultEnabledSlots;", WEB)
        self.assertIn('"Mesure 9"', WEB)
        self.assertIn('"Mesure 10"', WEB)
        self.assertIn("0U,\n    0U,\n};\nconstexpr const char* kWaveshareDashboardDefaultLabels", WEB)
        sensor = WEB.split("void sendWaveshareDashboardSlotsResponse_(", 1)[1].split(
            "void sendWaveshareAlarmDashboardSlotsResponse_", 1
        )[0]
        self.assertIn("i < kWaveshareDashboardSensorSlotCount", sensor)

    def test_web_filters_inactive_and_keeps_slot_order(self):
        fetch = UI.split("async function fetchPoolSondeSlots()", 1)[1].split(
            "function poolSondeRangeFromEntry", 1
        )[0]
        self.assertIn("slot.enabled === true", fetch)
        self.assertIn("slot.slot < 10", fetch)
        self.assertIn(".sort((a, b) => a.slot - b.slot)", fetch)
        self.assertNotIn(".slice(0, 8)", fetch)

    def test_grid_renders_enabled_entries_not_eight_placeholders(self):
        grid = UI.split("function buildPoolSondeSlotsGrid(slots)", 1)[1].split(
            "function runtimeMeasureResolvedLabel", 1
        )[0]
        self.assertIn("slot.enabled === true", grid)
        self.assertIn("for (const slot of activeSlots)", grid)
        self.assertIn("Aucune sonde activée", grid)
        self.assertNotIn("Array(8)", grid)
        self.assertNotIn("i < 8", grid)


if __name__ == "__main__":
    unittest.main()

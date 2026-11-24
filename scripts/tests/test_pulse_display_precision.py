#!/usr/bin/env python3
"""Regression guards for pulse frequency / converted-flow presentation precision.

The full C++ ValueRegistry formatting test is in test/host/value_pipeline.cpp.
Run: python3 -m unittest scripts/tests/test_pulse_display_precision.py
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
IO = (ROOT / "src/Modules/IOModule/IOModule.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "src/Modules/IOModule/IOModule.h").read_text(encoding="utf-8")


class PulsePresentationPrecisionTests(unittest.TestCase):
    def test_frequency_has_two_display_decimals_in_both_modes(self):
        self.assertIn("kPulseRateDisplayPrecision = 2;", IO)
        self.assertEqual(
            IO.count("rateMeta.precision = kPulseRateDisplayPrecision;"), 2
        )

    def test_converted_flow_uses_configured_precision_in_both_modes(self):
        self.assertIn("int8_t appliedFlowPrecision = -1;", HEADER)
        self.assertIn(
            "dataStore_->values.setPrecision(ValueIds::ConvertedRate + s.logicalIdx, s.appliedFlowPrecision);",
            IO,
        )
        self.assertIn("s.appliedFlowPrecision = flowMeta.precision;", IO)
        self.assertIn("flowMeta.precision = sanitizeAnalogPrecision_(cfg.precision);", IO)
        self.assertEqual(
            IO.count("values.setPrecision(ValueIds::ConvertedRate + slot.logicalIdx, flowPrecision);"),
            2,
        )

    def test_raw_values_are_not_rounded_before_registry_write(self):
        self.assertIn("rate.f = slot.pulse.rate;", IO)
        self.assertIn("rate.f = measurement.pulsesPerMinute;", IO)
        self.assertIn("flow.d = converted;", IO)
        self.assertIn("values.write(ValueIds::ConvertedRate + slot.logicalIdx, flow,", IO)

    def test_web_mqtt_and_tft_use_shared_precision(self):
        web = (ROOT / "src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp").read_text(encoding="utf-8")
        mqtt = (ROOT / "src/Modules/Stores/DataStoreModule/DataStoreModule.cpp").read_text(encoding="utf-8")
        tft = (ROOT / "src/Modules/TFTModuleS3/TFTModuleS3.cpp").read_text(encoding="utf-8")
        self.assertIn("metadata.precision, &metadata", web)
        self.assertIn("formatValueSnapshot(out, len, d->id, value, meta, true)", mqtt)
        self.assertIn("readRuntimePrecision_(runtimeId, precision)", tft)


if __name__ == "__main__":
    unittest.main()

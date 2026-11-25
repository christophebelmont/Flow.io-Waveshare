"""Host-side circulation gate simulations for PoolLogic water temperature telemetry.

Run: python -m unittest scripts.tests.test_water_temp_publication
No ESP32 hardware, network, or PlatformIO required; the C++ gate is compiled
from the production service header, and publication integration is checked.
"""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WaterTemperatureGateTest(unittest.TestCase):
    def test_gate_compiles_and_handles_edge_cases(self):
        harness = r"""
#include <Core/Services/IPoolConfiguration.h>
#include <assert.h>
#include <limits>
int main() {
    // Configuration off preserves existing behavior.
    static_assert(poolWaterTemperatureCanPublish(false, false, false));
    static_assert(poolWaterTemperatureCanPublish(false, true, false));
    // Enabled: either filtration or a positive flow restores publication.
    static_assert(!poolWaterTemperatureCanPublish(true, false, false));
    static_assert(poolWaterTemperatureCanPublish(true, true, false));
    static_assert(poolWaterTemperatureCanPublish(true, false, true));
    static_assert(poolWaterTemperatureCanPublish(true, true, true));

    constexpr uint64_t now = 50000;
    assert(poolFreshPositiveFlow(true, true, 3.3, 49000, now));
    assert(poolFreshPositiveFlow(true, true, 0.001, 49000, now));
    assert(!poolFreshPositiveFlow(false, true, 3.3, 49000, now));  // no meter
    assert(!poolFreshPositiveFlow(true, false, 3.3, 49000, now));  // invalid
    assert(!poolFreshPositiveFlow(true, true, 0.0, 49000, now));   // stopped
    assert(!poolFreshPositiveFlow(true, true, -1.0, 49000, now));  // negative
    assert(!poolFreshPositiveFlow(true, true, std::numeric_limits<double>::quiet_NaN(), 49000, now));
    assert(!poolFreshPositiveFlow(true, true, std::numeric_limits<double>::infinity(), 49000, now));
    assert(!poolFreshPositiveFlow(true, true, 3.3, 0, now));      // no timestamp
    assert(!poolFreshPositiveFlow(true, true, 3.3, 39999, now));  // stale
    assert(!poolFreshPositiveFlow(true, true, 3.3, 50001, now));  // future timestamp
    assert(poolFreshPositiveFlow(true, true, 3.3, 40000, now));   // 10s boundary
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="flowio-water-temp-") as tmp:
            source = Path(tmp) / "gate.cpp"
            program = Path(tmp) / "gate"
            source.write_text(harness)
            build = subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                 "-Isrc", str(source), "-o", str(program)],
                cwd=ROOT, capture_output=True, text=True
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(program)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_mqtt_omits_blocked_temperature_without_zero_or_null(self):
        cpp = (ROOT / "src/Modules/IOModule/IOModule.cpp").read_text()
        start = cpp.index("const bool publicationAllowed = waterTempPublicationAllowed_(ep->numericId);")
        end = cpp.index("if (ep->numericId >= IO_ID_DO_BASE", start)
        block = cpp[start:end]
        self.assertIn('doc["available"] = value.valid && publicationAllowed;', block)
        self.assertIn("if (publicationAllowed) {", block)
        self.assertNotIn('doc["value"] = 0', block)
        self.assertIn('doc["value"] = value.v.f', block)
        self.assertIn('dataStore_->notifyChanged(DataKey(DATAKEY_IO_BASE + runtimeIndex));', cpp)
        # Model the resulting shape, not an artificial zero-temperature reading.
        for active in (False, True):
            sample = {"available": active}
            if active:
                sample["value"] = 24.6
            wire = json.loads(json.dumps(sample))
            self.assertEqual("value" in wire, active)
            if not active:
                self.assertFalse(wire["available"])

    def test_ui_french_and_english_and_default_off(self):
        prefix = ROOT / "src/Modules/PoolLogicModule"
        docs = json.loads((prefix / "text/cfgdocs.fr.json").read_text())["docs"]
        field = "poollogic/sensors/wat_temp_only_circulating"
        self.assertEqual(docs[field]["type"], "Bool")
        for locale in ("fr", "en"):
            strings = json.loads((prefix / ("text/i18n." + locale + ".json")).read_text())["translations"]
            self.assertIn("cfgdocs.poollogic.sensors.wat_temp_only_circulating.label", strings)
        cpp = (prefix / "PoolLogicLifecycle.cpp").read_text()
        header = (prefix / "PoolLogicModule.h").read_text()
        self.assertIn("cfg.registerVar(waterTempCirculationOnlyVar_, kCfgModuleId, kCfgBranchSensors);", cpp)
        self.assertIn("bool waterTempCirculationOnly_ = false;", header)
        self.assertIn('"wat_temp_io_id", "poollogic/sensors"', header)


    def test_shared_source_gates_web_mqtt_hmi_and_rf(self):
        io = (ROOT / "src/Modules/IOModule/IOModule.cpp").read_text()
        iio = (ROOT / "src/Core/Services/IIO.h").read_text()
        pool = (ROOT / "src/Modules/PoolLogicModule/PoolLogicControl.cpp").read_text()
        web = (ROOT / "src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp").read_text()
        rf = (ROOT / "src/Modules/HMIModule/Drivers/TfaVeniceRf433Sink.cpp").read_text()

        # One source-level gate controls both DataStore presentation paths.
        self.assertIn("publish ? ValueQuality::Valid : ValueQuality::Invalid", io)
        self.assertIn("if (publish) (void)setIoEndpointFloat(", io)
        self.assertIn("else (void)setIoEndpointInvalid(", io)
        self.assertIn("const bool publish = waterTempPublicationAllowed_(slot.ioId);", io)
        self.assertIn("updatePublished(policy.ioId, allowed);", io)
        self.assertIn("if (!waterTempPublicationAllowed_(id)) return IO_ERR_NOT_READY;", io)

        # All public typed I/O reads are gated; physical acquisition remains
        # available to PoolLogic regulation, including while filtration is off.
        self.assertIn("IoStatus IOModule::ioReadAnalogRaw_(", io)
        self.assertIn("return ioReadAnalogRaw_(id, outValue, outTsMs, outSeq);", io)
        self.assertIn("(*readAnalogRaw)", iio)
        self.assertIn("ioSvc_->readAnalogRaw", pool)
        self.assertIn("ioSvc->readAnalog(ioSvc->ctx, waterTempIoId", rf)

        # Legacy pool.water_temp in the runtime manifest maps to a04.
        # The web endpoint now resolves the CURRENT configured water sensor.
        self.assertIn("waveshareConfiguredWaterTemperatureInput_(poolCfgSvc)", web)
        self.assertIn("sourceId = ValueIds::Analog +", web)
        self.assertIn("presentDataValueById(dataStore->values, actual, out, metadata)", web)

        # Physical analogue outputs and non-water sensors stay untouched.
        self.assertIn("slot.endpoint->update(rounded, true", io)
        self.assertIn("const IoId previouslyGatedId = waterTempGateLastIoId_;", io)
        self.assertIn("updatePublished(previouslyGatedId, true);", io)

    def test_both_supported_flowmeter_modes(self):
        io = (ROOT / "src/Modules/IOModule/IOModule.cpp").read_text()
        self.assertIn("mode != IO_DIGITAL_INPUT_COUNTER", io)
        self.assertIn("mode != IO_DIGITAL_INPUT_PULSES_PER_MINUTE", io)
        self.assertIn("flow.quality == ValueQuality::Valid", io)
        self.assertIn("poolFreshPositiveFlow(true, valid,", io)



if __name__ == "__main__":
    unittest.main()

"""Run the real allocation-free drivers/codecs and scheduler against bounded hardware fakes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CODEC = "src/Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.cpp"
DRIVER = "src/Modules/PoolDeviceModule/Drivers/PoolDeviceDriver.cpp"

class PoolActuatorTests(unittest.TestCase):
    def compile_and_run(self, main, sources, includes=()):
        with tempfile.TemporaryDirectory(prefix="flow-actuator-test-") as tmp:
            binary = Path(tmp) / "test"
            command = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-Itest/host/stubs", "-Isrc", *["-I" + str(p) for p in includes],
                       "test/host/" + main, *sources, "-o", str(binary)]
            for invocation in (command, [str(binary)]):
                result = subprocess.run(invocation, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_interlock_availability_and_diagnostics(self):
        self.compile_and_run("pool_interlock.cpp", [])

    def test_driver_transitions_and_wire_dialects(self):
        self.compile_and_run("pool_actuators.cpp", [DRIVER, CODEC])

    def test_bus_arbitration_cancellation_and_retries(self):
        self.compile_and_run("rs485_scheduler.cpp", [
            "src/Modules/IOModule/IOScheduler/Rs485TransactionScheduler.cpp", CODEC])

    def test_configuration_validation(self):
        self.compile_and_run("pool_config.cpp", [
            "src/Modules/PoolDeviceModule/Drivers/PoolDriverConfig.cpp", DRIVER, CODEC],
            [ROOT / ".pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src"])

if __name__ == "__main__":
    unittest.main()

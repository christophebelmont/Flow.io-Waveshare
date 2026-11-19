"""Compile the actual PoolLogic RS485 retry decision against a host fake."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from scripts.tests.test_io_boot_binding import function

ROOT = Path(__file__).resolve().parents[2]


class SharedRs485BootRetryTests(unittest.TestCase):
    def test_startup_then_last_writer_wins(self):
        module = ROOT / "src/Modules/PoolLogicModule"
        header = (module / "PoolLogicModule.h").read_text()
        control = (module / "PoolLogicControl.cpp").read_text()
        harness = (ROOT / "test/host/poollogic_shared_rs485_boot.cpp").read_text()
        harness = harness.replace("// DEVICE_FSM", function(header, "struct DeviceFsm") + ";")
        harness = harness.replace("// PRODUCTION_METHOD",
            function(control, "void PoolLogicModule::applyDeviceControl_"))
        with tempfile.TemporaryDirectory(prefix="flow-rs485-boot-") as directory:
            binary = Path(directory) / "test"
            main = Path(directory) / "main.cpp"
            main.write_text(harness)
            for cmd in (
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(main), "-o", str(binary)],
                [str(binary)]
            ):
                result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

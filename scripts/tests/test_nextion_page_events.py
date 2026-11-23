"""Exercise the production Nextion parser through a simulated UART."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NextionPageEventTests(unittest.TestCase):
    def test_navigation_and_context_resume(self):
        with tempfile.TemporaryDirectory(prefix="flow-nextion-test-") as directory:
            binary = Path(directory) / "test"
            build = [
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
                "-DARDUINOJSON_ENABLE_PROGMEM=0",
                "-Itest/host/nextion_stubs", "-Isrc", "-Iinclude",
                "-I.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src",
                "test/host/nextion_page_events.cpp",
                "src/Modules/HMIModule/Drivers/NextionDriver.cpp",
                "src/Modules/HMIModule/Drivers/NextionDisplayIdentity.cpp",
                "-o", str(binary),
            ]
            for command in (build, [str(binary)]):
                result = subprocess.run(command, cwd=ROOT, capture_output=True,
                                        text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

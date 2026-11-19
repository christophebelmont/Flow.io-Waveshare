"""Exercise the production DS18B20 driver and bus with a fake clock and probes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Ds18b20PollingTest(unittest.TestCase):
    def test_conversion_schedule_and_samples(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'test'
            subprocess.run([
                'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-Isrc', '-Iinclude',
                '-Itest/host/stubs', '-Itest/host/ds18b20_stubs',
                'test/host/ds18b20_polling.cpp',
                'src/Modules/IOModule/IODrivers/Ds18b20Driver.cpp',
                'src/Modules/IOModule/IOBus/OneWireBus.cpp',
                '-o', str(exe),
            ], cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

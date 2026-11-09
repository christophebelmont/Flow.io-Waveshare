"""Exercise production relay startup, equipment adoption and first write enable."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from scripts.tests.test_io_boot_binding import function

ROOT = Path(__file__).resolve().parents[2]


class PoolRelayBootTests(unittest.TestCase):
    def test_retained_relay_and_safety(self):
        module = ROOT / 'src/Modules/PoolDeviceModule'
        header = (module / 'PoolDeviceModule.h').read_text()
        source = (ROOT / 'test/host/pool_relay_boot.cpp').read_text()
        for marker, signature in (
            ('// DEVICE_TYPE', 'enum PoolDeviceType'),
            ('// DEVICE_DEFINITION', 'struct PoolDeviceDefinition'),
            ('// DEVICE_SLOT', 'struct PoolDeviceSlot'),
        ):
            source = source.replace(marker, function(header, signature) + ';')
        production = function((module / 'PoolDeviceRuntime.cpp').read_text(),
                              'bool PoolDeviceModule::configureRuntime_')
        control = (module / 'PoolDeviceControl.cpp').read_text()
        for signature in ('void PoolDeviceModule::tickDevices_',
                          'bool PoolDeviceModule::dependenciesSatisfied_',
                          'bool PoolDeviceModule::maxUptimeReached_',
                          'uint32_t PoolDeviceModule::toSeconds_',
                          'PoolDeviceSvcStatus PoolDeviceModule::svcSetWritesEnabledImpl_'):
            production += '\n' + function(control, signature)
        source = source.replace('// PRODUCTION_FUNCTIONS', production)
        with tempfile.TemporaryDirectory(prefix='flow-relay-boot-') as directory:
            main, binary = Path(directory) / 'main.cpp', Path(directory) / 'test'
            main.write_text(source)
            for command in (
                ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-Itest/host/stubs', '-Isrc', '-Iinclude',
                 '-I.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src', str(main),
                 str(module / 'Drivers/PoolDeviceDriver.cpp'),
                 'src/Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.cpp',
                 'src/Modules/IOModule/IOProtocols/Modbus/AquaGemRtuCodec.cpp', '-o', str(binary)],
                [str(binary)],
            ):
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

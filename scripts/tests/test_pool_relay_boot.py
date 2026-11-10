"""Exercise equipment startup policies, persistence and real drivers against fake hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from scripts.tests.test_io_boot_binding import function

ROOT = Path(__file__).resolve().parents[2]


class PoolRelayBootTests(unittest.TestCase):
    def test_equipment_boot_and_safety(self):
        module = ROOT / 'src/Modules/PoolDeviceModule'
        header = (module / 'PoolDeviceModule.h').read_text()
        source = (ROOT / 'test/host/pool_relay_boot.cpp').read_text()
        for marker, signature in (
            ('// DEVICE_TYPE', 'enum PoolDeviceType'),
            ('// DEVICE_DEFINITION', 'struct PoolDeviceDefinition'),
            ('// DEVICE_SLOT', 'struct PoolDeviceSlot'),
        ):
            source = source.replace(marker, function(header, signature) + ';')
        runtime = (module / 'PoolDeviceRuntime.cpp').read_text()
        production = '\n'.join(function(runtime, signature) for signature in (
            'void PoolDeviceModule::commitRuntimeState_', 'bool PoolDeviceModule::configureRuntime_'))
        runtime_helpers = (module / 'PoolDeviceRuntime.h').read_text()
        source = source.replace('// RUNTIME_STATE_HELPERS', '\n'.join(
            function(runtime_helpers, 'static inline bool ' + name) for name in (
                'samePoolDeviceTarget', 'samePoolDeviceFeedback', 'samePoolDeviceState', 'setPoolDeviceRuntimeState')))
        control = (module / 'PoolDeviceControl.cpp').read_text()
        for signature in ('void PoolDeviceModule::tickDevices_',
                          'bool PoolDeviceModule::dependenciesSatisfied_',
                          'bool PoolDeviceModule::maxUptimeReached_',
                          'uint32_t PoolDeviceModule::toSeconds_',
                          'PoolDeviceSvcStatus PoolDeviceModule::svcSetWritesEnabledImpl_'):
            production += '\n' + function(control, signature)
        production += '\n' + function(control, 'PoolDeviceSvcStatus PoolDeviceModule::setTarget_')
        startup = (module / 'PoolDeviceStartup.cpp').read_text()
        production += '\n' + startup[startup.index('bool PoolDeviceModule::restoreRunning_'):]
        source = source.replace('// PRODUCTION_FUNCTIONS', production)
        with tempfile.TemporaryDirectory(prefix='flow-relay-boot-') as directory:
            main, binary = Path(directory) / 'main.cpp', Path(directory) / 'test'
            main.write_text(source)
            for command in (
                ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-Itest/host/stubs', '-Itest/host', '-Isrc', '-Iinclude',
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

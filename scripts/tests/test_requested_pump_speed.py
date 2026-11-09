"""Exercise real ConfigStore typed writes and PoolDevice RPM restore/commands."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.tests.test_io_discovery_boot import function

ROOT = Path(__file__).resolve().parents[2]

class RequestedPumpSpeedTests(unittest.TestCase):
    def test_persistence_and_boot(self):
        source = (ROOT / 'test/host/requested_pump_speed_harness.h').read_text()
        config = (ROOT / 'src/Core/ConfigStore.h').read_text()
        for signature in ('bool ConfigStore::set(ConfigVariable<T, H>&',
                          'void ConfigStore::loadPersistentVar(ConfigVariable<T, H>&'):
            source += '\ntemplate<typename T, size_t H>\n' + function(config, signature)
        runtime = (ROOT / 'src/Modules/PoolDeviceModule/PoolDeviceRuntime.cpp').read_text()
        for signature in ('bool PoolDeviceModule::validateSpeedConfiguration_',
                          'bool PoolDeviceModule::restoreRequestedSpeed_'):
            source += '\n' + function(runtime, signature)
        control = (ROOT / 'src/Modules/PoolDeviceModule/PoolDeviceControl.cpp').read_text()
        start = control.index('bool PoolDeviceModule::configurationSetpoint_')
        end = control.index('PoolDeviceSvcStatus PoolDeviceModule::svcReadStateImpl_', start)
        source += '\n' + control[start:end]
        source += (ROOT / 'test/host/requested_pump_speed_scenarios.cpp').read_text()
        with tempfile.TemporaryDirectory(prefix='flow-requested-speed-') as directory:
            main = Path(directory) / 'main.cpp'
            binary = Path(directory) / 'test'
            main.write_text(source)
            commands = [
                ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-Itest/host/stubs', '-Isrc', '-Iinclude',
                 '-I.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src', str(main),
                 'src/Modules/PoolDeviceModule/Drivers/PoolDeviceDriver.cpp',
                 'src/Modules/PoolDeviceModule/Drivers/PoolDriverConfig.cpp',
                 'src/Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.cpp',
                 'src/Modules/IOModule/IOProtocols/Modbus/AquaGemRtuCodec.cpp', '-o', str(binary)],
                [str(binary)]]
            for command in commands:
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()

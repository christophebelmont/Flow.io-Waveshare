"""Compile production lease arbitration, commands, migration and discovery against bounded host services."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class OverrideCommandTests(unittest.TestCase):
    def test_production_manager(self):
        source = (ROOT / 'test/host/pool_overrides_harness.h').read_text()
        for file in ('PoolDeviceOverrides.cpp', 'PoolDeviceOverrideHa.cpp'):
            body = (ROOT / 'src/Modules/PoolDeviceModule' / file).read_text()
            source += '\n' + '\n'.join(line for line in body.splitlines() if not line.startswith(('#include', '#define')))
            if file == 'PoolDeviceOverrides.cpp':
                source += '\n#include "Modules/PoolDeviceModule/PoolDeviceOverrideDiscovery.h"\n'
        control = (ROOT / 'src/Modules/PoolDeviceModule/PoolDeviceControl.cpp').read_text()
        begin = control.index('bool PoolDeviceModule::configurationSetpoint_')
        end = control.index('PoolDeviceSvcStatus PoolDeviceModule::svcReadStateImpl_', begin)
        source += '\n' + control[begin:end]
        source += (ROOT / 'test/host/pool_override_adapter_harness.h').read_text()
        adapter = (ROOT / 'src/Modules/PoolLogicModule/PoolLogicOverrides.cpp').read_text()
        source += adapter[adapter.index('bool PoolLogicModule::handleOverride_('):]
        source += (ROOT / 'test/host/pool_overrides_scenarios.cpp').read_text()
        with tempfile.TemporaryDirectory() as directory:
            main = Path(directory) / 'main.cpp'
            binary = Path(directory) / 'test'
            main.write_text(source)
            for command in ([
                'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-pragma-once-outside-header', '-Isrc',
                '-I.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src', str(main), '-o', str(binary)], [str(binary)]):
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()

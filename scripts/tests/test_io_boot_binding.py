"""Execute production boot binding, capability validation and migration with a hardware inventory."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class BootBindingTest(unittest.TestCase):
    def test_boot_resolution_validation_and_migration(self):
        module = (ROOT / 'src/Modules/IOModule/IOModule.cpp').read_text()
        production = '\n'.join(function(module, signature) for signature in (
            'bool IOModule::resolveAnalogBinding_', 'bool IOModule::resolveDigitalInputBinding_',
            'bool IOModule::resolveDigitalOutputBinding_', 'int32_t IOModule::sanitizeAnalogPrecision_',
            'void IOModule::refreshAnalogConfigState_'))
        boot = (ROOT / 'src/Modules/IOModule/IOBootBinding.cpp').read_text()
        migration = (ROOT / 'src/Modules/IOModule/IOBindingMigration.cpp').read_text()
        production += '\n' + boot[boot.index('IOPortPresence IOModule::'):]
        production += '\n' + migration[migration.index('void IOModule::'):]
        harness = (ROOT / 'test/host/io_boot_binding.cpp').read_text()
        with tempfile.TemporaryDirectory() as tmp:
            cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            cpp.write_text(harness.replace('// PRODUCTION_FUNCTIONS', production))
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-DADS1X15_GAIN_6144MV=0',
                            '-Isrc', '-Iinclude', '-Itest/host/value_stubs',
                            '-I.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src',
                            str(cpp), '-o', str(exe)], cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

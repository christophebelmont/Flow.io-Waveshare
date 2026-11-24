"""Run the actual PoolLogic arbitration, migration and transitions with host services."""
from pathlib import Path
import subprocess
import tempfile
import json
import re
import unittest
from scripts.tests.test_io_boot_binding import function

ROOT = Path(__file__).resolve().parents[2]

class PoolLogicModesTests(unittest.TestCase):
    def test_home_assistant_select_templates(self):
        text = (ROOT / 'src/Modules/PoolLogicModule/PoolLogicLifecycle.cpp').read_text()
        for name in ('operatingModeSelect', 'phCorrectionSelect'):
            begin = text.index('const HASelectEntry ' + name + '{')
            block = text[begin:text.index('};', begin)]
            templates = re.findall(r'R"\((.*?)\)"', block)
            self.assertEqual(len(templates), 2)
            for template in templates:
                # publishSelect embeds these strings in Discovery JSON directly.
                self.assertIsInstance(json.loads('"' + template + '"'), str)

    def test_modes_transitions_and_migration(self):
        module = ROOT / 'src/Modules/PoolLogicModule'
        header = (module / 'PoolLogicModule.h').read_text()
        control = (module / 'PoolLogicControl.cpp').read_text()
        modes = (module / 'PoolLogicModes.cpp').read_text()
        source = (ROOT / 'test/host/poollogic_modes_harness.h').read_text()
        source = source.replace('// PRODUCTION_TYPES', header[header.index('    enum DisinfectionType'):header.index('    // ConfigStore stores')])
        source = source.replace('// PRODUCTION_STATE', header[header.index('    // ConfigStore stores'):header.index('    ConfigVariable<uint8_t,0> operatingModeVar_')])
        begin = header.index('    ConfigVariable<uint8_t,0> operatingModeVar_')
        end = header.index('    ConfigVariable<uint8_t,0> swgControlModeVar_', begin)
        source = source.replace('// PRODUCTION_CONFIG', header[begin:end])
        constants = control[control.index('constexpr float kHeaterHysteresisC'):control.index('const char* poolDeviceSvcStatusStr_')]
        source = source.replace('// CONTROL_CONSTANTS', constants + '\nconstexpr uint16_t o2WeekKeyFromDayKey_(uint16_t day) { return day / 7U; }')
        signatures = ['void PoolLogicModule::runControlLoop_', 'void PoolLogicModule::syncDeviceState_',
                      'void PoolLogicModule::resetTemporalPidState_', 'bool PoolLogicModule::stepTemporalPid_',
                      'bool PoolLogicModule::stepO2Protocol_']
        production = '\n'.join(function(control, signature) for signature in signatures)
        production += '\n' + '\n'.join(function(modes, signature) for signature in [
            'bool PoolLogicModule::validateModes_', 'bool PoolLogicModule::migrateModes_',
            'bool PoolLogicModule::serviceManualControlAllowed_', 'void PoolLogicModule::reconcileModes_'])
        commands = (module / 'PoolLogicCommands.cpp').read_text()
        helpers = '\n'.join(function(commands, signature) for signature in (
            'static bool parseCmdArgsObject_', 'static void writeCmdError_'))
        source = source.replace('// COMMAND_HELPERS', helpers)
        production += '\n' + function(commands, 'bool PoolLogicModule::cmdOperatingModeSet_')
        source = source.replace('// PRODUCTION_METHODS', production)
        source += (ROOT / 'test/host/poollogic_modes_scenarios.cpp').read_text()
        with tempfile.TemporaryDirectory(prefix='flow-pool-modes-') as folder:
            main, binary = Path(folder) / 'main.cpp', Path(folder) / 'test'
            main.write_text(source)
            for command in ([
                'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-const-variable',
                '-fsanitize=address,undefined', '-Itest/host/stubs', '-Isrc', '-Iinclude',
                '-I.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src', str(main), '-o', str(binary)], [str(binary)]):
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()

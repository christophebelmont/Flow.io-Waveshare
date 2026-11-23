#!/usr/bin/env python3
"""Compile the production rate estimator and flow conversion with a host compiler."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[2]

class PulseFlowRateTests(unittest.TestCase):
    def test_two_inputs_four_units(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / 'pulse-rate-only'
            subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g',
                            '-Isrc', 'test/host/pulse_rate_only/scenarios.cpp',
                            '-o', str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], cwd=ROOT, check=True)

    def test_wiring_and_output_data(self):
        module = (ROOT / 'src/Modules/IOModule/IOModule.cpp').read_text()
        ui = (ROOT / 'data/webinterface/app.js').read_text()
        catalog = (ROOT / 'src/Core/Values/catalog.json').read_text()
        assert 'IO_DIGITAL_INPUT_PULSES_PER_MINUTE' in module
        assert 'pulseFlowConvert(' in module
        assert 'pulseFlowUnitText(' in module
        assert 'if (!measurement.refreshed) return validRate;' in module
        assert 'ValueIds::ConvertedRate + slot.logicalIdx' in module
        assert "modeValue === 2 && isCountControl" in ui
        assert "modeValue !== 2 && isFlowUnit" in ui
        assert '"i01.flow"' in catalog

if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Exercise I2C address arbitration and verify UI/persistence coverage."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DRIVERS = ['ads1115_int', 'ads1115_ext', 'sht40', 'bmp280', 'bme680', 'ina226'] + [f'expander{i:02d}' for i in range(4)]

class I2cAddressTests(unittest.TestCase):
    def test_ads_conversion_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'ads1115'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g',
                            '-Itest/host/ads1115', '-Itest/host/ina226', '-Isrc',
                            'src/Modules/IOModule/IODrivers/Ads1115Driver.cpp',
                            'test/host/ads1115/scenarios.cpp', '-o', str(binary)],
                           cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)

    def test_startup_readiness(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'i2c-startup'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g', '-Isrc',
                            'test/host/i2c_startup_readiness.cpp', '-o', str(binary)],
                           cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)

    def test_arbitration(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'i2c-addresses'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g', '-Isrc',
                            'test/host/i2c_address_selection.cpp', '-o', str(binary)],
                           cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)

    def test_ina226_identity_and_timeout(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'ina226'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g', '-Itest/host/ina226', '-Isrc',
                            'src/Modules/IOModule/IODrivers/Ina226Driver.cpp',
                            'test/host/ina226/scenarios.cpp', '-o', str(binary)],
                           cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)

    def test_ui_address_fields(self):
        base = ROOT / 'src/Modules/IOModule/text'
        docs = json.loads((base / 'cfgdocs.fr.json').read_text())['docs']
        forms = json.loads((base / 'cfgmods.fr.json').read_text())['docs']
        for locale in ['fr', 'en']:
            strings = json.loads((base / f'i18n.{locale}.json').read_text())['translations']
            for driver in DRIVERS:
                for field in ['address', 'secondary_address']:
                    path = f'io/drivers/{driver}/{field}'
                    self.assertEqual(docs[path]['type'], 'UInt8')
                    self.assertEqual(forms[path]['display_format'], 'hex')
                    for key in ['label_t', 'help_t']:
                        self.assertTrue(strings[docs[path][key]])

if __name__ == '__main__':
    unittest.main()

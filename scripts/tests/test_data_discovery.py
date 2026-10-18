"""Exercise production catalogue discovery across two boot-time selections."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
class DataDiscoveryTests(unittest.TestCase):
    def test_selected_sensors_and_removals(self):
        source = (ROOT/'src/Profiles/Waveshare/WaveshareIoAssembly.cpp').read_text()
        a=source.index('void syncDataSensors('); b=source.index('\nvoid syncSwitches',a)
        code=(ROOT/'test/host/data_discovery.cpp').read_text().replace('// PRODUCTION_SYNC',source[a:b])
        with tempfile.TemporaryDirectory() as tmp:
            cpp=Path(tmp)/'discovery.cpp'; exe=Path(tmp)/'discovery';cpp.write_text(code)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-Itest/host/value_stubs','-Isrc','-Iinclude',str(cpp),
                'src/Core/Values/ValueRegistry.cpp','src/Core/Values/ValueExpression.cpp','-o',str(exe)],cwd=ROOT,check=True)
            subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()

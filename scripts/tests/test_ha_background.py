"""Exercise HA background admission and single-task completion/cleanup."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from scripts.tests.test_io_discovery_boot import function
ROOT = Path(__file__).resolve().parents[2]

class HaBackgroundTests(unittest.TestCase):
    def test_admission_refresh_and_cleanup(self):
        source = (ROOT / 'src/Modules/Network/HAModule/HAModule.cpp').read_text()
        names = ['setStartupReady', 'requestAutoconfigRefresh', 'prepareDiscovery_',
                 'enqueuePending_', 'discoveryReady_', 'markAllPending_', 'isPending_',
                 'setPending_', 'anyPending_', 'entityCount_', 'onMessagePublished_', 'onMessageDropped_', 'loop']
        methods = []
        for name in names:
            marker = 'HAModule::' + name + '('
            start = source.index(marker)
            methods.append(function(source, source[source.rfind('\n', 0, start) + 1:start] + marker))
        fixture = (ROOT / 'test/host/ha_background.cpp').read_text()
        code = fixture.replace('// PRODUCTION_METHODS', '\n'.join(methods))
        for one_shot in (0, 1):
            with self.subTest(one_shot=one_shot), tempfile.TemporaryDirectory(prefix='flow-ha-admission-') as tmp:
                cpp = Path(tmp) / 'test.cpp'; exe = Path(tmp) / 'test'
                cpp.write_text(code)
                subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', f'-DFLOW_HA_ONESHOT_DISCOVERY={one_shot}',
                                '-Isrc', str(cpp), '-o', str(exe)], cwd=ROOT, check=True)
                subprocess.run([str(exe)], check=True)

if __name__ == '__main__': unittest.main()

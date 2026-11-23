"""Exercise runtime admission, silence deadlines and changes during publication."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from scripts.tests.test_io_discovery_boot import function

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / 'src/Modules/Network/MQTTModule'

class RuntimeRefreshTests(unittest.TestCase):
    def test_admission_refresh_reconnection_and_concurrent_changes(self):
        source = (MODULE / 'RuntimeProducer.cpp').read_text()
        names = ['markRoutePending_', 'enqueueRoute_', 'offerBackground', 'onConnected',
                 'beginFullSync_', 'tick', 'onDataChanged', 'buildMessage',
                 'onMessagePublished', 'onMessageDropped',
                 'completeInitialFullSnapshotRoute_', 'notifyInitialFullSnapshotComplete_']
        methods = []
        for name in names:
            marker = 'RuntimeProducer::' + name + '('
            start = source.index(marker)
            signature = source[source.rfind('\n', 0, start) + 1:start] + marker
            methods.append(function(source, signature))
        production = '\n'.join(methods)
        header = (MODULE / 'RuntimeProducer.h').read_text()
        declaration = header[header.index('class RuntimeProducer {'):].replace('private:', 'public:')
        fixture = (ROOT / 'test/host/mqtt_runtime.cpp').read_text()
        code = fixture.replace('// PRODUCTION_CLASS', declaration).replace('// PRODUCTION_METHODS', production)
        with tempfile.TemporaryDirectory(prefix='flow-runtime-admission-') as tmp:
            cpp = Path(tmp) / 'test.cpp'
            exe = Path(tmp) / 'test'
            cpp.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-Itest/host/value_stubs',
                            '-Isrc', '-Iinclude', str(cpp), '-o', str(exe)], cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__': unittest.main()

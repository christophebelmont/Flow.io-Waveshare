"""Serialize the production catalog response into its bounded PSRAM transport buffer."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class DataCatalogResponseTests(unittest.TestCase):
    def test_catalog_serialization_and_transport(self):
        source = (ROOT / 'src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp').read_text()
        start = source.index('struct WebJsonBuffer final')
        buffer = source[start:source.index('\n};', start) + 3]
        start = source.index('    server_.on("/api/data/values"')
        end = source.index('    server_.on("/api/history/value"', start)
        route = source[start:end]
        self.assertNotIn('beginResponseStream', route)
        self.assertIn('sendBufferedJson(request, body)', route)
        start = route.index('        auto body =')
        build = route[start:route.index('        sendBufferedJson(request, body);')]
        harness = (ROOT / 'test/host/data_catalog_response.cpp').read_text()
        harness = harness.replace('// PRODUCTION_BUFFER', buffer).replace('// PRODUCTION_BUILD', build)
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / 'response.cpp'
            exe = Path(tmp) / 'response'
            cpp.write_text(harness)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-Isrc', '-Iinclude',
                            '-I.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src',
                            str(cpp), '-o', str(exe)], cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()

"""Exercise the production metadata response with the installed ArduinoJson library."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WebMetaTests(unittest.TestCase):
    def test_metadata_response_with_local_remote_and_missing_display(self):
        source = (ROOT / 'src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp').read_text()
        start = source.index('        StaticJsonDocument<', source.index('server_.on("/api/web/meta"'))
        end = source.index('\n    });', start)
        harness = (ROOT / 'test/host/web_meta.cpp').read_text().replace('// PRODUCTION_HANDLER', source[start:end])
        includes = ROOT / '.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src'
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / 'meta.cpp'
            binary = Path(tmp) / 'meta'
            cpp.write_text(harness)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I' + str(includes),
                            '-I' + str(ROOT / 'src'), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

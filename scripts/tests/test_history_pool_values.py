"""Serialize the production history value list into its bounded PSRAM transport buffer."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class HistoryPoolValuesTests(unittest.TestCase):
    def test_history_value_list(self):
        source = (ROOT / 'src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp').read_text()
        start = source.index('struct WebJsonBuffer final')
        buffer = source[start:source.index('\n};', start) + 3]
        route_start = source.index('    server_.on("/api/history/pool"')
        route_end = source.index('    server_.on("/api/data/values"', route_start)
        route = source[route_start:route_end]
        alloc_start = route.index('        constexpr size_t kDayBytes')
        body_line = route.index('        auto body = std::make_shared<WebJsonBuffer>', alloc_start)
        alloc = route[alloc_start:route.index('\n', body_line) + 1]
        values_start = route.index('        bool first = true;')
        values = route[values_start:route.index('        body->print("]}");', values_start)]
        harness = (ROOT / 'test/host/history_pool_values.cpp').read_text()
        harness = harness.replace('// PRODUCTION_BUFFER', buffer).replace(
            '// PRODUCTION_ALLOC', alloc).replace('// PRODUCTION_VALUES', values)
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / 'history_pool_values.cpp'
            exe = Path(tmp) / 'history_pool_values'
            cpp.write_text(harness)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-Isrc', '-Iinclude',
                            '-I.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src',
                            str(cpp), '-o', str(exe)], cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()

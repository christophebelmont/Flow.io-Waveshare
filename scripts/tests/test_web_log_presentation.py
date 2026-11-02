"""Check the bounded runtime log replay path and shared HMI value contract."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WebLogPresentationTests(unittest.TestCase):
    def test_runtime_log_replay_is_bounded_and_cursor_based(self):
        header = (ROOT / 'src/Modules/Network/WebInterfaceModule/WebInterfaceModule.h').read_text()
        server = (ROOT / 'src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp').read_text()
        websocket = (ROOT / 'src/Modules/Network/WebInterfaceModule/WebInterfaceWs.cpp').read_text()
        self.assertIn('kRuntimeLogHistoryCapacity = 4096U', header)
        self.assertIn('heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)', server)
        self.assertIn('runtimeLogHistoryNextSequence_', header)
        self.assertIn('server_.on("/api/logs/runtime"', server)
        self.assertIn('request->beginResponse(\n        "application/json", response->length()', server)
        self.assertIn('[response](uint8_t* buffer, size_t maxLen, size_t index)', server)
        self.assertIn('formatLogEntryLine_(ctx->self, e, line, sizeof(line), true)', server)
        self.assertIn('out.print("\\\\u001b")', server)
        self.assertIn('\\"lost\\"', server)
        self.assertIn('id="runtimelog"', server)
        self.assertIn('/api/logs/runtime?after=', server)
        self.assertIn('out.textContent = "";', server)
        self.assertIn('setCloseClientOnQueueFull(false)', websocket)
        self.assertIn('wsFlowEnqueuedCount_', websocket)
        self.assertIn('web request refused count=%lu', server)

    def test_shared_value_model_keeps_freshness_metadata(self):
        view = (ROOT / 'src/Core/Values/DataValueView.h').read_text()
        hmi = (ROOT / 'src/Modules/HMIModule/HMIModule.cpp').read_text()
        self.assertIn('enum class DataValueFreshness : uint8_t { Unknown, Valid, Degraded, Stale }', view)
        self.assertIn('uint64_t sampledAtMs', view)
        self.assertIn('uint32_t ageMs', view)
        self.assertIn('presentDataValueById', view)
        self.assertIn('readSharedAnalogValue_', hmi)

    def test_measurement_freshness_states(self):
        view = (ROOT / 'src/Core/Values/DataValueView.h').read_text()
        start = view.index('enum class DataValueFreshness')
        end = view.index('\n/** Descriptor lookup', start)
        production = view[start:end]
        harness = r'''
#include <cassert>
#include <cstdint>
enum class ValueQuality : uint8_t { Unknown, Valid, Invalid };
// PRODUCTION
int main() {
    assert(classifyDataValueFreshness(false, ValueQuality::Unknown, 0, 1000) == DataValueFreshness::Unknown);
    assert(classifyDataValueFreshness(true, ValueQuality::Valid, 100, 1000) == DataValueFreshness::Valid);
    assert(classifyDataValueFreshness(true, ValueQuality::Invalid, 100, 1000) == DataValueFreshness::Degraded);
    assert(classifyDataValueFreshness(true, ValueQuality::Valid, 1001, 1000) == DataValueFreshness::Stale);
    assert(classifyDataValueFreshness(true, ValueQuality::Invalid, 5000, 0) == DataValueFreshness::Degraded);
}
'''.replace('// PRODUCTION', production)
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / 'freshness.cpp'
            exe = Path(tmp) / 'freshness'
            cpp.write_text(harness)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

"""Check that disabling HTTP timing preserves watchdog activity notifications."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PerformanceDiagnosticsTests(unittest.TestCase):
    def test_http_activity_without_timing(self):
        source = (ROOT / 'src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp').read_text()
        start = source.index('struct HttpLatencyScope {')
        scope = source[start:source.index('\n};', start) + 3]
        fixture = r'''
#include "Core/PerformanceDiagnostics.h"
#include <cassert>
#include <cstdint>
#define FLOW_WEB_HEAP_FORENSICS 0
struct AsyncWebServerRequest { int method() const { return 0; } };
constexpr uint32_t kHttpLatencyInfoMs = 40, kHttpLatencyWarnMs = 1000;
enum : unsigned { MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2 };
unsigned clockReads = 0, activityCalls = 0, messages = 0;
uint32_t clockUs = 100;
uint32_t micros() { ++clockReads; return clockUs; }
unsigned heap_caps_get_free_size(unsigned) { return 10000; }
unsigned heap_caps_get_largest_free_block(unsigned) { return 8000; }
const char* httpMethodName_(int) { return "GET"; }
void activity(void*) { ++activityCalls; }
void (*gHttpActivityHook)(void*) = activity;
void* gHttpActivityHookCtx = nullptr;
void logMessage(const char*, ...) { ++messages; }
#define LOGI(...) logMessage(__VA_ARGS__)
#define LOGW(...) logMessage(__VA_ARGS__)
// PRODUCTION_SCOPE
int main() {
    AsyncWebServerRequest request;
    {
        HttpLatencyScope scope(&request, "/api/test");
        assert(activityCalls == 1);
        clockUs += 2000000;
    }
#if FLOW_PERFORMANCE_DIAGNOSTICS
    assert(clockReads == 2 && messages == 1);
#else
    assert(clockReads == 0 && messages == 0);
#endif
}
'''
        for enabled in (False, True):
            with self.subTest(diagnostics=enabled), tempfile.TemporaryDirectory(prefix='flow-perf-') as tmp:
                cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
                cpp.write_text(fixture.replace('// PRODUCTION_SCOPE', scope))
                subprocess.run([
                    'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-Isrc',
                    *(['-DFLOW_PERFORMANCE_DIAGNOSTICS=1'] if enabled else []),
                    str(cpp), '-o', str(exe)
                ], cwd=ROOT, check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

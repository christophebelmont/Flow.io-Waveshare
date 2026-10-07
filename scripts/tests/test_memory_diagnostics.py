"""Exercise the production tracer's async lifetime, bounds and log analysis."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('analyze_memory', ROOT / 'scripts/analyze_memory.py')
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class MemoryDiagnosticsTests(unittest.TestCase):
    def test_serial_ansi_colors(self):
        text = ('[I][webinterface] \x1b[32mWM b=abcdef01 t=10 id=1 e=enter '
                'i=20000 il=9000 im=8000 p=7000000 pl=6000000 a=1\x1b[0m\r\n'
                '[I][webinterface] \x1b[32mWMR b=abcdef01 id=1 method=GET path=/api/test\x1b[0m\r\n')
        report = analysis.analyze(text)
        request = report['boots']['abcdef01']['incomplete'][0]
        self.assertEqual(request['route'], 'GET /api/test')
        self.assertEqual(request['id'], 1)

    def test_replay_reboots_partial_requests_and_clock_wrap(self):
        def row(t, rid, phase, boot='abcdef01'):
            return f'WM b={boot} t={t} id={rid} e={phase} i=20000 il=9000 im=8000 p=7000000 pl=6000000 a=1'
        enter = row(4294967290, 1, 'enter')
        text = '\n'.join([enter, enter, row(4, 1, 'released'), row(5, 2, 'ready'),
                          row(8, 1, 'enter', 'abcdef02'),
                          'WMR b=abcdef01 id=1 method=GET path=/api/test',
                          'Stack PoolHistory/history@c1 min=3508/6144B',
                          'Stack PoolHistory/history@c1 min=4000/6144B'])
        report = analysis.analyze(text)
        self.assertEqual(report['duplicate_samples'], 1)
        self.assertEqual(len(report['boots']), 2)
        first = report['boots']['abcdef01']
        self.assertEqual(first['completed'][0]['duration_ms'], 10)
        self.assertEqual(first['completed'][0]['route'], 'GET /api/test')
        self.assertEqual(len(first['incomplete']), 1)
        self.assertEqual(report['stacks']['PoolHistory/history@c1']['minimum_free'], 3508)
        self.assertFalse(analysis.analyze('WM b=123 t=10 id=1 e=enter i=2')['boots'])

    def test_compiler_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'test.su').write_text('x.cpp:1:1:void a()\t48\tstatic\n'
                                          'x.cpp:2:1:void b()\t100\tdynamic,bounded\n')
            self.assertEqual([r['bytes'] for r in analysis.stack_usage(tmp)], [100, 48])

    def test_production_lifetime_and_capacity(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'Core').mkdir()
            (path / 'Arduino.h').write_text('#pragma once\n#include <cstdint>\ninline uint32_t millis() { static uint32_t t=0; return ++t; }\n')
            (path / 'esp_system.h').write_text('#pragma once\ninline unsigned esp_random() { return 42; }\n')
            (path / 'esp_heap_caps.h').write_text('''#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_INTERNAL=1, MALLOC_CAP_SPIRAM=2, MALLOC_CAP_8BIT=4;
extern bool failAllocation;
extern unsigned internalFree;
inline void* heap_caps_malloc(size_t n, unsigned) { return failAllocation ? nullptr : malloc(n); }
inline size_t heap_caps_get_free_size(unsigned c) { return (c & 1) ? internalFree : 7000000; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return 9000; }
inline size_t heap_caps_get_minimum_free_size(unsigned) { return 8000; }
''')
            (path / 'Core/ModuleLog.h').write_text('''#pragma once
void logMessage(const char*, ...);
#define LOGI(...) logMessage(__VA_ARGS__)
#define LOGW(...) logMessage(__VA_ARGS__)
''')
            (path / 'ESPAsyncWebServer.h').write_text('''#pragma once
#include <functional>
#include <string>
extern unsigned internalFree;
using ArMiddlewareNext=std::function<void()>;
struct AsyncWebServerRequest {
    std::function<void()> disconnect;
    bool response=false;
    ~AsyncWebServerRequest() { if (response) internalFree += 100; }
    void onDisconnect(std::function<void()> fn) { disconnect=fn; }
    const char* methodToString() { return "GET"; }
    std::string url() { return "/api/test"; }
};
struct AsyncWebServer {
    std::function<void(AsyncWebServerRequest*, ArMiddlewareNext)> middleware;
    void addMiddleware(decltype(middleware) fn) { middleware=fn; }
};
''')
            harness = r'''
#include "WebMemoryDiagnostics.h"
#include <ESPAsyncWebServer.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
bool failAllocation=false;
unsigned internalFree=20000;
std::vector<std::string> messages;
void logMessage(const char* format, ...) {
    char line[512]; va_list args; va_start(args, format);
    vsnprintf(line, sizeof(line), format, args); va_end(args);
    assert(std::string(line).size() < 160); messages.emplace_back(line);
}
unsigned count(const std::string& text) {
    unsigned n=0; for (const auto& s: messages) if (s.find(text)!=std::string::npos) ++n;
    return n;
}
int main() {
    AsyncWebServer server;
    failAllocation=true; WebMemoryDiagnostics::install(server);
    assert(!server.middleware); // No internal RAM fallback on failed PSRAM allocation.
    failAllocation=false; WebMemoryDiagnostics::install(server);
    auto request=std::make_unique<AsyncWebServerRequest>();
    server.middleware(request.get(), [&] { request->response=true; internalFree-=100; });
    assert(count("e=ready")==1 && count("e=released")==0);
    request->disconnect();
    assert(count("e=disconnect")==1 && count("e=released")==0);
    request.reset();
    assert(count("e=released")==1);
    assert(messages.back().find("i=20000 ")!=std::string::npos);
    assert(messages.back().find("a=0")!=std::string::npos);
    std::vector<std::unique_ptr<AsyncWebServerRequest>> requests;
    unsigned handled=0;
    for (unsigned i=0; i<25; ++i) {
        requests.emplace_back(new AsyncWebServerRequest);
        server.middleware(requests.back().get(), [&] { ++handled; });
    }
    assert(handled==25 && count("e=capacity")==1);
    requests.clear(); // Upgrades/destruction with no disconnect are also reclaimed.
    assert(count("e=released")==25);
    request.reset(new AsyncWebServerRequest);
    server.middleware(request.get(), [] {});
    request.reset();
    assert(count("e=released")==26 && count("e=capacity")==1);
    assert(messages.back().find("a=0")!=std::string::npos);
}
'''
            source = ROOT / 'src/Modules/Network/WebInterfaceModule'
            (path / 'main.cpp').write_text(harness)
            command = ['clang++', '-std=c++17', '-fsanitize=address,undefined',
                       '-DFLOW_MEMORY_DIAGNOSTICS=1', '-I'+tmp, '-I'+str(source),
                       str(source/'WebMemoryDiagnostics.cpp'), str(path/'main.cpp'),
                       '-o', str(path/'test')]
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([str(path/'test')], check=True, capture_output=True, text=True)
            # Disabled instrumentation compiles without Arduino or web dependencies.
            subprocess.run(['clang++', '-std=c++17', '-c', str(source/'WebMemoryDiagnostics.cpp'),
                            '-o', str(path/'disabled.o')], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()

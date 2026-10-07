"""Production PSRAM owners: failure, callback ownership and destruction through base pointers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PsramOwnershipTests(unittest.TestCase):
    def test_routes_and_typed_storage(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'esp_heap_caps.h').write_text(r'''
#pragma once
#include <cstdlib>
#include <cassert>
#include <set>
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
inline bool failAllocation=false;
inline unsigned attempts=0;
inline std::set<void*> allocated;
inline void* heap_caps_malloc(size_t size, unsigned caps) {
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)); ++attempts;
    if (failAllocation) return nullptr;
    auto p=malloc(size); assert(p); allocated.insert(p); return p;
}
inline void heap_caps_free(void* p) { assert(allocated.erase(p)==1); free(p); }
''')
            (root / 'ESPAsyncWebServer.h').write_text(r'''
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
using AsyncURIMatcher=std::string;
using WebRequestMethodComposite=unsigned;
using ArRequestHandlerFunction=std::function<void()>;
using ArUploadHandlerFunction=std::function<void()>;
using ArBodyHandlerFunction=std::function<void()>;
struct AsyncWebHandler { virtual ~AsyncWebHandler()=default; };
struct AsyncCallbackWebHandler : AsyncWebHandler {
    std::string uri; unsigned method=0;
    std::function<void()> request,upload,body;
    void setUri(std::string v) { uri=std::move(v); }
    void setMethod(unsigned v) { method=v; }
    void onRequest(std::function<void()> v) { request=std::move(v); }
    void onUpload(std::function<void()> v) { upload=std::move(v); }
    void onBody(std::function<void()> v) { body=std::move(v); }
};
struct AsyncWebServer {
    explicit AsyncWebServer(uint16_t) {}
    std::vector<std::unique_ptr<AsyncWebHandler>> handlers;
    void addHandler(AsyncWebHandler* h) { handlers.emplace_back(h); }
    void reset() { handlers.clear(); }
};
''')
            (root / 'WebHandlerImpl.h').write_text('#pragma once\n#include <ESPAsyncWebServer.h>\n')
            (root / 'main.cpp').write_text(r'''
#include "Core/SpiRamObject.h"
#include "Modules/Network/WebInterfaceModule/PsramWebServer.h"
#include <array>
#include <cassert>
struct State {
    static int live;
    int value;
    explicit State(int v) : value(v) { ++live; }
    ~State() { --live; }
};
int State::live=0;
int main() {
    {
        auto state=makeSpiRamObject<State>(42);
        assert(state && state->value==42 && State::live==1);
        auto moved=std::move(state);
        assert(!state && moved->value==42);
        auto bytes=makeSpiRamObject<std::array<unsigned char, 512>>();
        for (auto v : *bytes) assert(v==0);
        failAllocation=true;
        auto failed=makeSpiRamObject<State>(7);
        assert(!failed && State::live==1);
        failAllocation=false;
    }
    assert(State::live==0 && allocated.empty());
    {
        PsramWebServer server(80);
        assert(!server.routesReady());
        unsigned requests=0,uploads=0,bodies=0;
        auto capture=std::make_shared<int>(42);
        std::weak_ptr<int> weak=capture;
        assert(server.on("/upload",3,[capture,&requests] { assert(*capture==42); ++requests; },
                         [&uploads] { ++uploads; },[&bodies] { ++bodies; }));
        capture.reset();
        assert(!weak.expired() && server.routesReady() && server.routeCount()==1);
        auto* route=static_cast<AsyncCallbackWebHandler*>(server.handlers[0].get());
        assert(route->uri=="/upload" && route->method==3);
        route->request(); route->upload(); route->body();
        assert(requests==1 && uploads==1 && bodies==1);
        failAllocation=true;
        assert(!server.on("/failed",1,[] {}));
        assert(!server.routesReady() && server.routeCount()==1);
        failAllocation=false;
        unsigned before=attempts;
        assert(!server.on("/must-not-register",1,[] {}));
        assert(attempts==before); // A partial route set cannot become ready again.
        server.resetRoutes();
        assert(allocated.empty() && weak.expired() && !server.routesReady());
        for (unsigned i=0; i<100; ++i) assert(server.on("/route",1,[] {}));
        assert(server.routesReady() && server.routeCount()==100);
        assert(allocated.size()==100);
    } // Base-owned virtual destruction must call the matching heap_caps_free.
    assert(allocated.empty());
}
''')
            subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                            '-I'+tmp, '-I'+str(ROOT/'src'), str(root/'main.cpp'),
                            '-o', str(root/'test')], check=True, capture_output=True, text=True)
            subprocess.run([str(root/'test')], check=True, capture_output=True, text=True)


if __name__ == '__main__':
    unittest.main()

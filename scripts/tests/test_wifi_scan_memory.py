"""Run the production scan serializer with the real ArduinoJson PSRAM allocator."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WifiScanMemoryTests(unittest.TestCase):
    def test_json_snapshot_and_allocation_failure(self):
        source = (ROOT/'src/Modules/Network/WifiModule/WifiModule.cpp').read_text()
        start = source.index('bool WifiModule::buildScanStatusJson_(')
        function = source[start:source.index('\n}', start)+2]
        harness = r'''
#include "Core/SpiRamJsonDocument.h"
#include <cassert>
#include <cstring>
constexpr unsigned WIFI_AUTH_OPEN=0;
namespace Limits { namespace Wifi { namespace Buffers { constexpr size_t ScanStatusJson=4096; } } }
#define portENTER_CRITICAL(p) (++critical)
#define portEXIT_CRITICAL(p) (--critical)
struct WifiModule {
    static constexpr uint8_t kScanMaxResults=8;
    struct WifiScanEntry { char ssid[33]; int16_t rssi; uint8_t auth; bool hidden; };
    WifiScanEntry scanEntries_[kScanMaxResults]{};
    int scanMux_=0;
    bool scanRunning_=false,scanRequested_=false,scanHasResults_=true;
    uint8_t scanCount_=2,scanTotalFound_=2;
    int16_t scanLastError_=0;
    uint32_t scanLastStartMs_=123,scanLastDoneMs_=456;
    uint16_t scanGeneration_=7;
    bool buildScanStatusJson_(char* out,size_t outLen);
};
'''+function+r'''
int main() {
    WifiModule wifi;
    strcpy(wifi.scanEntries_[0].ssid,"quoted \"ssid\"");
    wifi.scanEntries_[0].rssi=-42;
    wifi.scanEntries_[0].auth=0;
    strcpy(wifi.scanEntries_[1].ssid,"protected");
    wifi.scanEntries_[1].rssi=-65;
    wifi.scanEntries_[1].auth=3;
    wifi.scanEntries_[1].hidden=true;
    char out[4096]{};
    assert(wifi.buildScanStatusJson_(out,sizeof(out)));
    assert(critical==0 && allocations==frees && allocations>0);
    ArduinoJson::DynamicJsonDocument doc(4096);
    assert(!deserializeJson(doc,out));
    assert(doc["generation"]==7 && doc["count"]==2);
    assert(doc["started_ms"]==123 && doc["updated_ms"]==456);
    assert(doc["networks"][0]["ssid"]=="quoted \"ssid\"");
    assert(doc["networks"][0]["secure"]==false);
    assert(doc["networks"][1]["rssi"]==-65 && doc["networks"][1]["hidden"]==true);
    assert(doc["networks"][1]["secure"]==true);
    failAllocation=true;
    assert(!wifi.buildScanStatusJson_(out,sizeof(out)));
    assert(critical==0 && allocations==frees);
    failAllocation=false;
    assert(!wifi.buildScanStatusJson_(out,2));
    assert(!wifi.buildScanStatusJson_(nullptr,0));
    wifi.scanCount_=0; wifi.scanRunning_=true; wifi.scanHasResults_=false;
    assert(wifi.buildScanStatusJson_(out,sizeof(out)));
    assert(!deserializeJson(doc,out));
    assert(doc["networks"].size()==0 && doc["running"]==true && doc["has_results"]==false);
    assert(allocations==frees);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path/'esp_heap_caps.h').write_text(r'''
#pragma once
#include <cstdlib>
#include <cassert>
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
inline bool failAllocation=false;
inline unsigned critical=0,allocations=0,frees=0;
inline void* heap_caps_malloc(size_t n,unsigned c) {
    assert(critical==0 && c==3);
    if (failAllocation) return nullptr;
    ++allocations; return malloc(n);
}
inline void heap_caps_free(void* p) { if (p) { ++frees; free(p); } }
inline void* heap_caps_realloc(void* p,size_t n,unsigned) { assert(critical==0); return realloc(p,n); }
''')
            (path/'main.cpp').write_text(harness)
            lib = ROOT/'.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src'
            build = subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined',
                                    '-I'+tmp,'-I'+str(ROOT/'src'),'-I'+str(lib),
                                    str(path/'main.cpp'),'-o',str(path/'test')],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run = subprocess.run([str(path/'test')],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)


if __name__ == '__main__':
    unittest.main()

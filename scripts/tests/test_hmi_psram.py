"""Exercise production UDP packet construction/retry against its new PSRAM buffers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start)+2]


def structure(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n    };', start)+7]


class HmiPsramTests(unittest.TestCase):
    def test_packets_queue_retry_and_missing_storage(self):
        module = ROOT/'src/Modules/Network/HmiUdpServerModule'
        source = (module/'HmiUdpServerModule.cpp').read_text()
        header = (module/'HmiUdpServerModule.h').read_text()
        protocol = (ROOT/'src/Core/Hmi/HmiUdpProtocol.h').read_text()
        protocol_source = (ROOT/'src/Core/Hmi/HmiUdpProtocol.cpp').read_text()
        types = protocol[protocol.index('enum class HmiUdpMsgType'):protocol.index('struct HmiUdpHelloPayload')]
        types += '\n#pragma pack(pop)\n'
        methods = ['bool begin()',
                   'bool sendImmediate_(HmiUdpMsgType type, const void* payload, uint8_t payloadLen, uint8_t flags)',
                   'bool enqueueReliable_(HmiUdpMsgType type, const void* payload, uint8_t payloadLen)',
                   'bool sendReliableLarge_(HmiUdpMsgType type, const void* payload, size_t payloadLen)',
                   'bool buildReliablePending_(HmiUdpMsgType type, const void* payload, size_t payloadLen)',
                   'bool loadNextReliable_()', 'void serviceReliableTx_(uint32_t nowMs)',
                   'void clearReliableQueue_(bool clearPending)',
                   'bool isConfigMsg_(HmiUdpMsgType type) const', 'bool sendAck_(uint16_t seq)']
        code = r'''
#include "Core/SpiRamObject.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <vector>
#define LOGD(...) ((void)0)
#define LOGW(...) ((void)0)
constexpr size_t HMI_UDP_MAX_PACKET=512;
constexpr uint16_t HMI_UDP_PORT=42110;
constexpr uint8_t HMI_UDP_MAGIC0='F', HMI_UDP_MAGIC1='H', HMI_UDP_VERSION=7;
constexpr uint8_t HMI_UDP_FLAG_ACK_REQUIRED=1, HMI_UDP_FLAG_IS_ACK=2;
uint32_t millis() { return 1; }
'''+types+r'''
uint16_t hmiUdpCrc16(const uint8_t* data, size_t len, uint16_t seed=0xffff);
static constexpr size_t kCrcOffset=offsetof(HmiUdpHeader, crc);
'''
        for signature in ['static uint16_t packetCrc_', 'uint16_t hmiUdpCrc16(',
                          'bool hmiUdpBuildPacket(', 'bool hmiUdpValidatePacket(']:
            code += function(protocol_source, signature)+'\n'
        code += r'''
struct Udp {
    unsigned starts=0;
    std::vector<std::vector<uint8_t>> packets;
    bool begin(uint16_t) { ++starts; return true; }
    bool beginPacket(int,int) { return true; }
    size_t write(const uint8_t* p,size_t n) { packets.emplace_back(p,p+n); return n; }
    int endPacket() { return 1; }
};
class HmiUdpServerModule {
public:
    static constexpr uint8_t HMI_UDP_OUT_QUEUE_SIZE=12, HMI_UDP_OUT_PAYLOAD_MAX=64;
    static constexpr size_t HMI_UDP_LARGE_PAYLOAD_MAX=HMI_UDP_MAX_PACKET-sizeof(HmiUdpHeader);
    static constexpr uint32_t ReliableRetryMs=150;
    static constexpr uint8_t ReliableMaxAttempts=7;
'''+structure(header, '    struct OutPacket {')+'\n'+structure(header, '    struct PacketBuffers {')+r'''
    SpiRamPtr<PacketBuffers> buffers_;
    Udp udp_;
    bool started_=false, displayOnline_=true;
    int remoteIp_=0, remotePort_=0;
    uint8_t outHead_=0, outTail_=0, reliableAttempts_=0;
    uint16_t txSeq_=1, lastRxSeq_=0, reliablePendingSeq_=0;
    size_t reliablePendingLen_=0;
    HmiUdpMsgType reliablePendingType_=HmiUdpMsgType::Error;
    uint32_t reliableLastSendMs_=0;
    bool wifiConnected_() const { return true; }
'''
        code += '\n'.join('    '+m+';' for m in methods)+'\n};\n'
        for m in methods:
            result, rest = m.split(' ', 1)
            code += function(source, result+' HmiUdpServerModule::'+rest)+'\n'
        code += r'''
void checkPacket(const std::vector<uint8_t>& data, const std::vector<uint8_t>& expected) {
    const HmiUdpHeader* h=nullptr; const uint8_t* payload=nullptr;
    assert(hmiUdpValidatePacket(data.data(),data.size(),h,payload));
    assert(h->len==expected.size());
    assert(memcmp(payload,expected.data(),expected.size())==0);
}
int main() {
    HmiUdpServerModule hmi;
    assert(!hmi.begin() && hmi.udp_.starts==0);
    assert(!hmi.sendAck_(1));
    hmi.buffers_=makeSpiRamObject<HmiUdpServerModule::PacketBuffers>();
    assert(hmi.begin() && hmi.begin() && hmi.udp_.starts==1);
    std::vector<uint8_t> small(64,0x42);
    assert(hmi.sendImmediate_(HmiUdpMsgType::HomeText,small.data(),small.size(),0));
    checkPacket(hmi.udp_.packets.back(),small);
    assert(hmi.enqueueReliable_(HmiUdpMsgType::HomeStateBits,small.data(),4));
    small[0]=99;
    assert(hmi.enqueueReliable_(HmiUdpMsgType::HomeStateBits,small.data(),4));
    assert(hmi.outHead_==1); // Existing state packet is coalesced, not duplicated.
    for (unsigned n=0; n<10; ++n)
        assert(hmi.enqueueReliable_(HmiUdpMsgType::HomeAlarmBits,small.data(),64));
    assert(!hmi.enqueueReliable_(HmiUdpMsgType::HomeAlarmBits,small.data(),64));
    hmi.serviceReliableTx_(1);
    checkPacket(hmi.udp_.packets.back(),std::vector<uint8_t>(small.begin(),small.begin()+4));
    auto packet=hmi.udp_.packets.back();
    unsigned sent=hmi.udp_.packets.size();
    hmi.serviceReliableTx_(150); assert(hmi.udp_.packets.size()==sent);
    hmi.serviceReliableTx_(151); assert(hmi.udp_.packets.back()==packet);
    hmi.clearReliableQueue_(true);
    std::vector<uint8_t> large(HmiUdpServerModule::HMI_UDP_LARGE_PAYLOAD_MAX,0xab);
    assert(hmi.sendReliableLarge_(HmiUdpMsgType::ConfigViewSnapshot,large.data(),large.size()));
    checkPacket(hmi.udp_.packets.back(),large);
    assert(hmi.udp_.packets.back().size()==512);
    sent=hmi.udp_.packets.size();
    assert(hmi.sendReliableLarge_(HmiUdpMsgType::ConfigViewSnapshot,large.data(),large.size()));
    assert(hmi.udp_.packets.size()==sent); // Duplicate does not overwrite pending storage.
    large.push_back(0);
    assert(!hmi.sendReliableLarge_(HmiUdpMsgType::ConfigViewSnapshot,large.data(),large.size()));
    for (unsigned n=1; n<=7; ++n) hmi.serviceReliableTx_(1+150*n);
    assert(hmi.reliablePendingLen_==0 && hmi.outHead_==hmi.outTail_);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path/'esp_heap_caps.h').write_text('''#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
inline void* heap_caps_malloc(size_t n,unsigned) { return malloc(n); }
inline void heap_caps_free(void* p) { free(p); }
''')
            (path/'main.cpp').write_text(code)
            build = subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined',
                                    '-I'+tmp,'-I'+str(ROOT/'src'),str(path/'main.cpp'),
                                    '-o',str(path/'test')], capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run = subprocess.run([str(path/'test')], capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)


if __name__ == '__main__':
    unittest.main()

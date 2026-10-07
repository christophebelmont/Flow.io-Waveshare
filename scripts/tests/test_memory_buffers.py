"""Exercise production PSRAM buffers and config-producer allocation with ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


class MemoryBufferTests(unittest.TestCase):
    def test_allocation_and_async_lifetime(self):
        web = (ROOT / 'src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp').read_text()
        mqtt = ROOT / 'src/Modules/Network/MQTTModule'
        header = (mqtt / 'MqttConfigRouteProducer.h').read_text()
        source = (mqtt / 'MqttConfigRouteProducer.cpp').read_text()
        buffer = web[web.index('struct WebJsonBuffer final'):web.index('\n};', web.index('struct WebJsonBuffer final')) + 3]
        state = header[header.index('    struct RouteState {'):header.index('    RouteState* state_')]
        production = '\n'.join(function(source, signature) for signature in (
            'MqttConfigRouteProducer::~MqttConfigRouteProducer()',
            'bool MqttConfigRouteProducer::allocateRouteState_()'))
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_INTERNAL=2, MALLOC_CAP_8BIT=4;
bool psram=true, fail=false;
unsigned allocations=0, frees=0, lastCaps=0;
size_t heap_caps_get_total_size(unsigned) { return psram ? 1024 : 0; }
bool psramFound() { return psram; }
void* heap_caps_malloc(size_t size, unsigned caps) {
    ++allocations; lastCaps=caps; return fail ? nullptr : malloc(size);
}
void heap_caps_free(void* memory) { ++frees; free(memory); }
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
struct Print {
    virtual ~Print() = default;
    virtual size_t write(uint8_t)=0;
    virtual size_t write(const uint8_t*, size_t)=0;
};
class MqttConfigRouteProducer {
public:
    static constexpr uint8_t MaxRoutes=96;
    // STATE
    RouteState* state_ = nullptr;
    ~MqttConfigRouteProducer();
    bool allocateRouteState_();
};
'''
        scenarios = r'''
int main() {
    {
        MqttConfigRouteProducer producer;
        assert(producer.allocateRouteState_());
        assert(lastCaps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        assert(sizeof(*producer.state_) == 768);
        assert(!producer.state_->pendingFlags_[95] && !producer.state_->retryFirstRefusedMs_[95]);
        const auto count = allocations;
        producer.state_->pendingFlags_[95] = true;
        assert(producer.allocateRouteState_() && allocations == count);
        assert(producer.state_->pendingFlags_[95]);
    }
    assert(frees == 1);
    {
        MqttConfigRouteProducer producer;
        fail = true;
        const auto count=allocations;
        assert(!producer.allocateRouteState_() && !producer.state_);
        assert(allocations == count+1); // Do not consume internal reserve on PSRAM exhaustion.
        fail = false; psram = false;
        assert(producer.allocateRouteState_());
        assert(lastCaps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    psram = true;
    const auto priorFrees = frees;
    auto body = std::make_shared<WebJsonBuffer>(10001);
    std::vector<uint8_t> input(10000, 42), result(10000);
    assert(body->valid() && body->write(input.data(), input.size()) == input.size());
    assert(body->finish() && body->length()==input.size());
    auto response = [body](uint8_t* out, size_t count, size_t at) { return body->fillAt(out,count,at); };
    body.reset(); // HTTP owns the immutable page until transmission/cancellation completes.
    assert(frees == priorFrees);
    for (size_t at=0; at<result.size();) {
        const auto n=response(result.data()+at, 137, at);
        assert(n>0); at+=n;
    }
    assert(result==input && response(result.data(),1,result.size())==0);
    {
        WebJsonBuffer small(4);
        assert(small.write(input.data(),4)==0 && !small.finish() && small.overflowed());
        small.reset();
        assert(small.write(uint8_t('x'))==1 && small.finish());
    }
    fail=true;
    WebJsonBuffer absent(100);
    assert(!absent.valid() && !absent.finish());
}
'''
        code = harness.replace('// STATE', state) + buffer + production + scenarios
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / 'memory.cpp'
            cpp.write_text(code)
            exe = Path(tmp) / 'memory'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

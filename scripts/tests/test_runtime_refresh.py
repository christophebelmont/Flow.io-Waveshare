"""Per-sensor silence deadlines force non-retained publications without changing samples."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from scripts.tests.test_io_discovery_boot import function
ROOT = Path(__file__).resolve().parents[2]

class RuntimeRefreshTests(unittest.TestCase):
    def test_independent_deadlines_retries_and_wraparound(self):
        source = (ROOT/'src/Modules/Network/MQTTModule/RuntimeProducer.cpp').read_text()
        production = '\n'.join(function(source, signature) for signature in (
            'void RuntimeProducer::tick(', 'void RuntimeProducer::markRoutePending_(',
            'MqttBuildResult RuntimeProducer::buildMessage(',
            'void RuntimeProducer::onMessagePublished(', 'void RuntimeProducer::onMessageDropped('))
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
enum class MqttBuildResult { Ready, RetryLater, NoLongerNeeded, PermanentError };
enum class RuntimeRouteClass { NumericThrottled, ActuatorImmediate };
struct MqttBuildContext { char* topic; size_t topicCapacity; char* payload; size_t payloadCapacity;
    uint16_t topicLen{}, payloadLen{}; uint8_t qos{}; bool retain{}; };
uint32_t nowMs = 0;
uint32_t millis() { return nowMs; }
struct Provider {
    uint32_t sequence=1;
    const char* payload="{\"value\":700,\"available\":true}";
    const char* runtimeSnapshotSuffix(uint8_t) const { return "state"; }
    bool buildRuntimeSnapshot(uint8_t,char* out,size_t len,uint32_t& ts) const {
        snprintf(out,len,"%s",payload); ts=sequence; return true;
    }
};
struct MqttService { void* ctx{};
    void (*formatTopic)(void*,const char*,char*,size_t)=[](void*,const char* suffix,char* out,size_t len) {
        snprintf(out,len,"flow/test/%s",suffix);
    };
};
struct RuntimeProducer {
    struct Route { Provider* provider{}; uint8_t snapshotIdx{};
        RuntimeRouteClass routeClass=RuntimeRouteClass::NumericThrottled;
        bool pending=true,force=true,refreshQueued=false;
        uint32_t refreshIntervalMs=120000,lastPublishMs{},lastPublishedTs{},lastBuiltTs{};
        char suffix[32]="state";
    } routes_[3];
    uint8_t routeCount_=3;
    static constexpr uint32_t NumericThrottleMs=10000;
    const MqttService* mqttSvc_{};
    unsigned attempts[3]{}; bool accept=true;
    bool enqueueRoute_(uint8_t i) { ++attempts[i]; return accept; }
    void completeInitialFullSnapshotRoute_(uint8_t) {}
    void markRoutePending_(uint8_t,bool);
    void tick(uint32_t);
    void onMessagePublished(uint16_t);
    void onMessageDropped(uint16_t);
    MqttBuildResult buildMessage(uint16_t,MqttBuildContext&);
};
'''
        scenarios = r'''
int main() {
    RuntimeProducer producer; Provider providers[3]; MqttService service;
    for(unsigned i=0;i<3;++i) producer.routes_[i].provider=&providers[i];
    producer.routes_[2].refreshIntervalMs=0; // Actuators keep their existing policy.
    producer.mqttSvc_=&service;
    char topic[128], payload[256]; MqttBuildContext ctx{topic,sizeof(topic),payload,sizeof(payload)};
    auto build=[&](unsigned i) {
        assert(producer.buildMessage(i,ctx)==MqttBuildResult::Ready);
        assert(!ctx.retain && std::string(payload)==providers[i].payload);
    };
    nowMs=1000; build(0); producer.onMessagePublished(0);
    nowMs=5000; build(1); producer.onMessagePublished(1);
    producer.tick(120999); assert(producer.attempts[0]==0 && producer.attempts[1]==0);
    producer.tick(121000); assert(producer.attempts[0]==1 && producer.attempts[1]==0);
    producer.tick(121001); assert(producer.attempts[0]==1); // No duplicate while queued.
    nowMs=121010; build(0); producer.onMessagePublished(0); // Unchanged sequence still published.
    assert(providers[0].sequence==1);
    producer.tick(124999); assert(producer.attempts[1]==0);
    producer.tick(125000); assert(producer.attempts[1]==1);
    providers[1].payload="{\"value\":false,\"quality\":1}";
    nowMs=125000; build(1); producer.onMessagePublished(1);
    // Ordinary changed-value publications reset only that sensor's deadline.
    nowMs=200000; ++providers[0].sequence;
    producer.markRoutePending_(0,false); build(0); producer.onMessagePublished(0);
    producer.tick(241010); assert(producer.attempts[0]==1);
    // Queue saturation must not postpone the deadline; retry without duplicates.
    producer.accept=false; producer.tick(245000);
    assert(producer.attempts[1]==2 && !producer.routes_[1].refreshQueued);
    assert(producer.routes_[1].lastPublishMs==125000);
    producer.accept=true; producer.tick(245001); producer.tick(245002);
    assert(producer.attempts[1]==3);
    providers[1].payload="{\"value\":null,\"quality\":2}";
    nowMs=245003; build(1); producer.onMessagePublished(1);
    assert(providers[1].sequence==1); // Refresh never makes an invalid measurement valid.
    producer.tick(320000); assert(producer.attempts[0]==2);
    producer.onMessageDropped(0);
    assert(producer.routes_[0].lastPublishMs==200000);
    producer.tick(320001); assert(producer.attempts[0]==3);
    // Unsigned elapsed-time comparison also works across millis() wraparound.
    producer.routes_[0].lastPublishMs=UINT32_MAX-60000;
    producer.routes_[0].refreshQueued=false;
    producer.routes_[1].refreshIntervalMs=0;
    producer.tick(59998); assert(producer.attempts[0]==3);
    producer.tick(59999); assert(producer.attempts[0]==4);
    assert(producer.attempts[2]==0);
}

'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp=Path(tmp)/'test.cpp'; exe=Path(tmp)/'test'
            cpp.write_text(harness+production+scenarios)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__': unittest.main()

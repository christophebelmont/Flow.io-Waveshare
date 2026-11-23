#include "Core/Services/IMqtt.h"
#include "Core/RuntimeSnapshotProvider.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>
namespace Limits { constexpr size_t TopicBuf = 70; }
template<class... Args> void log(Args...) {}
#define LOGD(...) log(__VA_ARGS__)
#define LOGE(...) ((void)0)
uint32_t nowMs = 0;
uint32_t millis() { return nowMs; }
// PRODUCTION_CLASS
bool RuntimeProducer::ensureStorage_(uint16_t count) { return count <= storageCapacity_; }
// PRODUCTION_METHODS

struct Provider : IRuntimeSnapshotProvider {
    uint32_t sequence = 1;
    mutable std::function<void()> duringBuild;
    uint16_t runtimeSnapshotCount() const override { return 300; }
    const char* runtimeSnapshotSuffix(uint16_t) const override { return "state"; }
    RuntimeRouteClass runtimeSnapshotClass(uint16_t) const override { return RuntimeRouteClass::NumericThrottled; }
    bool runtimeSnapshotAffectsKey(uint16_t idx, DataKey key) const override { return idx == key; }
    bool buildRuntimeSnapshot(uint16_t, char* out, size_t len, uint32_t& ts) const override {
        snprintf(out, len, "{\"value\":700}"); ts = sequence;
        if (duringBuild) duringBuild();
        return true;
    }
};
struct Transport {
    bool connected = true, accept = true;
    unsigned immediate = 0, background = 0;
    std::vector<uint16_t> sent;
};
int main() {
    RuntimeProducer producer;
    RuntimeProducer::Route routes[300]; bool initialDone[300]{};
    producer.routes_ = routes; producer.initialFullSnapshotDone_ = initialDone;
    producer.routeCount_ = 3; producer.storageCapacity_ = 300;
    Provider providers[3]; Transport transport; MqttService service{};
    service.ctx = &transport;
    service.isConnected = [](void* ctx) { return static_cast<Transport*>(ctx)->connected; };
    service.formatTopic = [](void*, const char* suffix, char* out, size_t len) {
        snprintf(out, len, "flow/test/%s", suffix);
    };
    service.enqueue = [](void* ctx, uint8_t, uint16_t id, uint8_t priority, uint8_t) {
        auto& t = *static_cast<Transport*>(ctx); ++t.immediate;
        assert(priority == (uint8_t)MqttPublishPriority::High);
        if (t.accept) t.sent.push_back(id);
        return t.accept;
    };
    service.tryEnqueueBackground = [](void* ctx, uint8_t, uint16_t id, MqttPublishPriority priority) {
        auto& t = *static_cast<Transport*>(ctx); ++t.background;
        assert(priority == MqttPublishPriority::Normal);
        if (!t.accept) return MqttAdmissionResult::Deferred;
        t.sent.push_back(id); return MqttAdmissionResult::Accepted;
    };
    producer.mqttSvc_ = &service;
    for (unsigned i = 0; i < 3; ++i) {
        routes[i].provider = &providers[i]; routes[i].snapshotIdx = i;
        routes[i].refreshIntervalMs = 120000;
        strcpy(routes[i].suffix, "state");
    }
    routes[1].routeClass = RuntimeRouteClass::ActuatorImmediate;
    routes[2].refreshIntervalMs = 0;
    routes[2].pending = false; routes[2].force = false;
    char topic[128], payload[256]; MqttBuildContext ctx{};
    ctx.topic = topic; ctx.topicCapacity = sizeof(topic);
    ctx.payload = payload; ctx.payloadCapacity = sizeof(payload);
    auto publish = [&](uint16_t i) {
        assert(producer.buildMessage(i, ctx) == MqttBuildResult::Ready);
        assert(!ctx.retain); producer.onMessagePublished(i);
    };
    nowMs = 1000; publish(0);
    nowMs = 5000; publish(1);
    producer.tick(120999); assert(transport.sent.empty());
    nowMs = 121000; producer.tick(nowMs);
    assert(transport.sent.empty()); // Numeric deadlines only mark pending work.
    assert(producer.offerBackground());
    assert(!producer.offerBackground()); // No duplicate job while admitted.
    nowMs = 121010; publish(0); assert(providers[0].sequence == 1);
    nowMs = 125000; producer.tick(nowMs);
    assert(transport.immediate == 1); publish(1);
    nowMs = 200000; ++providers[0].sequence;
    producer.markRoutePending_(0, false); publish(0);
    nowMs = 241010; producer.tick(nowMs); assert(!routes[0].pending);
    transport.accept = false;
    nowMs = 245000; producer.tick(nowMs);
    assert(!routes[1].admitted && routes[1].lastPublishMs == 125000);
    transport.accept = true;
    producer.tick(++nowMs); producer.tick(++nowMs);
    assert(routes[1].admitted && transport.immediate == 3);
    publish(1);
    nowMs = 320000; producer.tick(nowMs);
    assert(producer.offerBackground());
    // A change arriving after payload construction survives completion.
    assert(producer.buildMessage(0, ctx) == MqttBuildResult::Ready);
    producer.markRoutePending_(0, false); ++providers[0].sequence;
    producer.onMessagePublished(0);
    assert(routes[0].pending && !routes[0].admitted);
    assert(!producer.offerBackground()); // Numeric throttle is checked before taking a slot.
    nowMs += 10000;
    assert(producer.offerBackground()); publish(0); assert(!routes[0].pending);
    // Also retain a change raised from inside the builder itself.
    producer.markRoutePending_(0, true);
    providers[0].duringBuild = [&] { ++providers[0].sequence; producer.markRoutePending_(0, false); };
    assert(producer.offerBackground()); publish(0);
    assert(routes[0].pending);
    providers[0].duringBuild = {}; nowMs += 10000;
    assert(producer.offerBackground()); publish(0);

    routes[0].lastPublishMs = UINT32_MAX - 60000;
    nowMs = 59998; producer.tick(nowMs); assert(!routes[0].pending);
    nowMs = 59999; producer.tick(nowMs); assert(routes[0].pending);
    assert(producer.offerBackground()); publish(0);
    assert(!routes[2].pending); // No refresh for event-only routes.
    // Route indexes are not limited to the 96-entry configuration mailbox.
    producer.routeCount_ = 300;
    routes[299].provider = &providers[0]; routes[299].snapshotIdx = 299;
    strcpy(routes[299].suffix, "state");
    assert(producer.offerBackground() && transport.sent.back() == 299); publish(299);
    if (routes[1].admitted) publish(1);
    const auto before = transport.sent.size();
    producer.onConnected();
    assert(transport.sent.size() == before); // Client task only signals the full sync.
    transport.connected = false; producer.tick(nowMs);
    assert(transport.sent.size() == before);
    transport.connected = true; producer.tick(nowMs);
    assert(routes[0].pending && routes[2].pending && routes[299].pending);
    assert(transport.sent.size() == before + 1); // Only the immediate actuator enters here.
    publish(1);
    assert(producer.offerBackground());
    const auto last = transport.sent.back(); publish(last);
    // DataChanged for a numeric route marks work without any direct admission.
    const auto admissions = transport.background;
    const auto stats = producer.onDataChanged(299);
    assert(stats.matched == 1 && transport.background == admissions);
    assert(routes[299].pending);

    RuntimeProducer startup;
    RuntimeProducer::Route startupRoutes[3]; bool startupDone[3]{};
    startup.routes_ = startupRoutes; startup.initialFullSnapshotDone_ = startupDone;
    startup.routeCount_ = startup.storageCapacity_ = 3; startup.mqttSvc_ = &service;
    unsigned completed = 0;
    startup.initialFullSnapshotCtx_ = &completed;
    startup.initialFullSnapshotCb_ = [](void* ctx) { ++*static_cast<unsigned*>(ctx); };
    for (unsigned i = 0; i < 3; ++i) {
        startupRoutes[i].provider = &providers[0];
        strcpy(startupRoutes[i].suffix, "state");
    }
    startup.onConnected(); startup.tick(nowMs);
    assert(completed == 0 && startup.initialFullSnapshotRemaining_ == 3);
    for (unsigned i = 0; i < 3; ++i) {
        assert(startup.offerBackground());
        const auto id = transport.sent.back();
        assert(startup.buildMessage(id, ctx) == MqttBuildResult::Ready);
        startup.onMessagePublished(id);
    }
    assert(completed == 1 && !startup.initialFullSnapshotActive_);
    // A newer forced request survives completion of a previously forced send.
    startup.markRoutePending_(0, true);
    assert(startup.offerBackground() && transport.sent.back() == 0);
    providers[0].duringBuild = [&] { startup.markRoutePending_(0, true); };
    assert(startup.buildMessage(0, ctx) == MqttBuildResult::Ready);
    startup.onMessagePublished(0);
    assert(startupRoutes[0].pending && startupRoutes[0].force);
    providers[0].duringBuild = {};
    assert(startup.offerBackground() && transport.sent.back() == 0);
    assert(startup.buildMessage(0, ctx) == MqttBuildResult::Ready);
    startup.onMessagePublished(0);
    assert(!startupRoutes[0].pending && !startupRoutes[0].force);
    startup.onConnected(); startup.tick(nowMs);
    assert(completed == 1 && startup.initialFullSnapshotRemaining_ == 3);
}

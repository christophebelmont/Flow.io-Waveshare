#include "Core/RuntimeSnapshotProvider.h"
#include "Core/Services/IMqtt.h"
#include "Core/DataKeys.h"
#include "Core/Generated/DataValueCatalog_Generated.h"
#include "Core/Values/ValueFormat.h"
#include "Core/Values/ValueRegistry.h"
#include <esp_heap_caps.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

template<class... Args> void logDebug(Args...) {}
#define LOGD(...) logDebug(__VA_ARGS__)
#define LOGE(...) ((void)0)
uint32_t nowMs = 10000;
uint32_t millis() { return nowMs; }
struct ConfigStore {};
struct ServiceRegistry {};
struct TestStore {
    ValueRegistry values;
    bool selected[ValueIds::Capacity]{};
    bool publishHa(ValueId id) const { return selected[id]; }
};
struct DataExposureConfig {
    void apply(TestStore& store) {
        for (const auto& d : kDataValues) store.selected[d.id] = d.defaultHa;
    }
};
class DataStoreModule : public IRuntimeSnapshotProvider {
public:
    // DATA_METHOD_DECLARATIONS
    RuntimeRouteClass runtimeSnapshotClass(uint16_t) const override { return RuntimeRouteClass::NumericThrottled; }
    uint32_t runtimeSnapshotRefreshMs(uint16_t) const override { return 120000; }
    ~DataStoreModule() { heap_caps_free(routes_); }
    const DataValueDescriptor** routes_ = nullptr;
    uint16_t routeCount_ = 0;
    DataExposureConfig exposure;
    DataExposureConfig* exposure_ = &exposure;
    TestStore _store;
};
// DATA_METHODS
// RUNTIME_CLASS
// RUNTIME_METHODS

struct IoProvider : IRuntimeSnapshotProvider {
    uint16_t runtimeSnapshotCount() const override { return 1; }
    const char* runtimeSnapshotSuffix(uint16_t) const override { return "rt/io/input/a00"; }
    RuntimeRouteClass runtimeSnapshotClass(uint16_t) const override { return RuntimeRouteClass::NumericThrottled; }
    bool runtimeSnapshotAffectsKey(uint16_t, DataKey) const override { return true; }
    bool buildRuntimeSnapshot(uint16_t, char* out, size_t len, uint32_t& ts) const override {
        ts = nowMs; snprintf(out, len, "{\"value\":24.5}"); return true;
    }
};

int main() {
    ConfigStore config; ServiceRegistry services;
    DataStoreModule data;
    assert(data._store.values.begin());
    RuntimeProducer runtime;
    IoProvider io;
    assert(runtime.registerProvider(&data)); // Same provider order as the board.
    assert(runtime.registerProvider(&io));
    data.onConfigLoaded(config, services);
    // All 39 counter/rate/flow defaults are selected, but none has a producer.
    unsigned selected = 0;
    for (const auto& d : kDataValues)
        if (d.kind == DataValueKind::Counter && data._store.publishHa(d.id)) ++selected;
    assert(selected == 39);
    data.onStart(config, services);
    runtime.rebuildRoutes(); // MQTT starts after all module config callbacks.

    struct Transport {
        std::vector<uint16_t> pending;
        bool completed = false;
    } transport;
    MqttService service{}; service.ctx = &transport;
    service.isConnected = [](void*) { return true; };
    service.formatTopic = [](void*, const char* suffix, char* out, size_t len) {
        snprintf(out, len, "flow/test/%s", suffix);
    };
    service.tryEnqueueBackground = [](void* ctx, uint8_t, uint16_t id, MqttPublishPriority) {
        auto& pending = static_cast<Transport*>(ctx)->pending;
        if (pending.size() >= 32) return MqttAdmissionResult::Deferred;
        pending.push_back(id); return MqttAdmissionResult::Accepted;
    };
    runtime.configure(&service, [](void* ctx) { static_cast<Transport*>(ctx)->completed = true; }, &transport);
    runtime.onConnected();
    bool ioPublished = false;
    char topic[128], payload[256]; MqttBuildContext ctx{};
    ctx.topic = topic; ctx.topicCapacity = sizeof(topic);
    ctx.payload = payload; ctx.payloadCapacity = sizeof(payload);
    for (unsigned cycle = 0; cycle < 200; ++cycle) {
        nowMs += 100;
        runtime.tick(nowMs);
        runtime.offerBackground();
        for (auto it = transport.pending.begin(); it != transport.pending.end();) {
            const auto result = runtime.buildMessage(*it, ctx);
            if (result == MqttBuildResult::RetryLater) { ++it; continue; }
            assert(result == MqttBuildResult::Ready);
            ioPublished |= strcmp(topic, "flow/test/rt/io/input/a00") == 0;
            runtime.onMessagePublished(*it);
            it = transport.pending.erase(it);
        }
    }
    assert(ioPublished); // Regressed: 32 undefined counters held every credit forever.
    assert(transport.completed && transport.pending.empty());
    // Changes and reconnects still reach IO after the initial snapshot.
    runtime.onDataChanged(DataKeys::IoBase);
    nowMs += 10000;
    runtime.tick(nowMs);
    assert(runtime.offerBackground() && transport.pending.size() == 1);
    assert(runtime.buildMessage(transport.pending.front(), ctx) == MqttBuildResult::Ready);
    assert(strcmp(topic, "flow/test/rt/io/input/a00") == 0);
    runtime.onMessagePublished(transport.pending.front());
    transport.pending.clear(); transport.completed = false;
    runtime.onConnected(); runtime.tick(nowMs);
    assert(runtime.offerBackground() && transport.pending.size() == 1);
    assert(runtime.buildMessage(transport.pending.front(), ctx) == MqttBuildResult::Ready);
    runtime.onMessagePublished(transport.pending.front());
    assert(transport.completed);
    heap_caps_free(runtime.routes_); heap_caps_free(runtime.initialFullSnapshotDone_);

    // Definitions arrive after DataStore's config callback, as in IOModule.
    DataStoreModule configured;
    assert(configured._store.values.begin());
    configured.onConfigLoaded(config, services);
    assert(configured.runtimeSnapshotCount() == 0);
    const ValueId count = ValueIds::Digital + 2;
    const ValueId rate = ValueIds::PulseRate + 2;
    const ValueId state = ValueIds::DigitalState + 3;
    assert(configured._store.values.define(count, {ValueType::UInt64, AggregationMode::Counter}));
    assert(configured._store.values.define(rate, {ValueType::Float, AggregationMode::Rate}));
    assert(configured._store.values.define(state, {ValueType::Bool}));
    assert(configured._store.values.define(ValueIds::Analog, {}));
    configured._store.selected[state] = true;
    configured._store.selected[ValueIds::Analog] = true; // Owned by the IO provider.
    configured._store.selected[count] = false; // Defined but publication disabled.
    configured.onStart(config, services);
    assert(configured.runtimeSnapshotCount() == 2);
    for (uint16_t i = 0; i < configured.runtimeSnapshotCount(); ++i) {
        const ValueId id = configured.routes_[i]->id;
        assert(id == rate || id == state);
        uint32_t ts = 0;
        assert(configured.buildRuntimeSnapshot(i, payload, sizeof(payload), ts));
        assert(strstr(payload, "\"value\":null")); // Defined, unsampled values still publish.
        assert(configured.runtimeSnapshotAffectsKey(i, DataKeys::ValueBase + id));
    }
    ValueNumber sample{}; sample.f = 12.5;
    assert(configured._store.values.write(rate, sample, 10, ValueQuality::Valid));
    uint32_t ts = 0;
    assert(configured.buildRuntimeSnapshot(0, payload, sizeof(payload), ts));
    assert(strstr(payload, "\"value\":12.5"));
    assert(configured._store.values.write(rate, sample, 20, ValueQuality::Invalid));
    assert(configured.buildRuntimeSnapshot(0, payload, sizeof(payload), ts));
    assert(strstr(payload, "\"value\":null")); // Sensor failures keep the route and its quality.
    puts("Catalogue runtime: undefined counters cannot block rt/io; late definitions and quality changes OK");
}

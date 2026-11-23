#include "Modules/Network/MQTTModule/PendingMqttRoutes.h"
#include "Core/EventBus/EventPayloads.h"
#include "Core/EventBus/EventId.h"
#include <cassert>
#include <functional>
#include <thread>
#include <vector>
#define LOGE(...) ((void)0)
struct Event { EventId id; const void* payload; };
struct FakeStore { bool ready = false; };
bool mqttReady(const FakeStore& store) { return store.ready; }
struct FakeService { FakeStore* store; };
class MqttConfigRouteProducer {
public:
    using CustomBuildFn = MqttBuildResult (*)(void*, uint16_t, MqttBuildContext&);
    // ROUTE
    static constexpr uint8_t MaxRoutes = PendingMqttRoutes::Capacity;
    Route routes_[3]{};
    uint8_t routeCount_ = 3, producerId_ = 47;
    bool producerRegistered_ = true, configLoaded_ = true, mqttReadyLatched_ = false;
    FakeService* dsSvc_ = nullptr;
    struct RouteState { bool pendingFlags_[MaxRoutes]{}; } state;
    RouteState* state_ = &state;
    const MqttService* mqttSvc_ = nullptr;
    PendingMqttRoutes requestedRoutes_;
    void requestFullSync(MqttPublishPriority);
    void refreshReadyGateAndMaybeSync_();
    void onTransportTick_(uint32_t);
    void onEvent_(const Event&);
    static MqttPublishPriority routePriority_(const Route&);
    bool offerBackground_();
    MqttAdmissionResult enqueueByRoute_(uint8_t, MqttPublishPriority);
    void setPending_(uint8_t,bool);
    bool isPending_(uint8_t) const;
    int8_t findRouteByMessage_(uint16_t) const;
    void onMessagePublished_(uint16_t);
    void onMessageDropped_(uint16_t);
};
// METHODS
int main() {
    FakeStore store; FakeService service{&store};
    MqttConfigRouteProducer producer; producer.dsSvc_ = &service;
    struct Transport {
        bool accept = true;
        std::vector<uint16_t> sent;
        std::vector<MqttPublishPriority> priorities;
        std::function<void()> duringAdmission;
    } transport;
    MqttService mqtt{}; mqtt.ctx = &transport;
    mqtt.tryEnqueueBackground = [](void* ctx, uint8_t, uint16_t id, MqttPublishPriority prio) {
        auto& t = *static_cast<Transport*>(ctx);
        if (t.duringAdmission) t.duringAdmission();
        if (!t.accept) return MqttAdmissionResult::Deferred;
        t.sent.push_back(id); t.priorities.push_back(prio);
        return MqttAdmissionResult::Accepted;
    };
    producer.mqttSvc_ = &mqtt;
    for (uint8_t i = 0; i < 3; ++i) {
        producer.routes_[i].messageId = i;
        producer.routes_[i].branch.moduleId = 5;
        producer.routes_[i].branch.localBranchId = i + 1;
    }
    producer.requestFullSync(MqttPublishPriority::Low);
    producer.onTransportTick_(1);
    assert(!producer.offerBackground_()); // Offline work remains pending.
    store.ready = true;
    producer.onTransportTick_(2);
    assert(transport.sent.empty()); // Preparation never fills the transport.
    transport.accept = false;
    for (unsigned i = 0; i < 100; ++i) {
        producer.onTransportTick_(i * 1000);
        assert(!producer.offerBackground_());
    } // More than 10 s of pressure cannot expire a configuration.
    transport.accept = true;
    for (unsigned i = 0; i < 3; ++i) assert(producer.offerBackground_());
    assert(transport.sent.size() == 3);
    assert(!producer.offerBackground_());
    ConfigChangedPayload payload{}; payload.moduleId = 5; payload.localBranchId = 2;
    producer.onEvent_({EventId::ConfigChanged, &payload});
    assert(!producer.offerBackground_()); // Already admitted; retain the newer request.
    producer.onMessagePublished_(1);
    assert(producer.offerBackground_() && transport.sent.back() == 1);
    producer.onMessagePublished_(0); producer.onMessageDropped_(1); producer.onMessagePublished_(2);
    assert(!producer.offerBackground_());
    store.ready = false; producer.onTransportTick_(6);
    store.ready = true; producer.onTransportTick_(7);
    for (unsigned i = 0; i < 3; ++i) assert(producer.offerBackground_());
    assert(transport.sent.size() == 7); // Reconnect resynchronizes every route.

    PendingMqttRoutes pending;
    pending.mark(95, MqttPublishPriority::Low);
    const auto eligible = [](uint8_t) { return true; };
    assert(!pending.drainOne(eligible, [&](uint8_t idx, MqttPublishPriority) {
        std::thread writer([&] { pending.mark(idx, MqttPublishPriority::High); });
        writer.join(); return MqttAdmissionResult::Deferred;
    }));
    assert(pending.drainOne(eligible, [&](uint8_t idx, MqttPublishPriority priority) {
        assert(idx == 95 && priority == MqttPublishPriority::High);
        std::thread writer([&] { pending.mark(idx, MqttPublishPriority::Normal); });
        writer.join(); return MqttAdmissionResult::Accepted;
    }));
    assert(pending.drainOne(eligible, [](uint8_t idx, MqttPublishPriority priority) {
        assert(idx == 95 && priority == MqttPublishPriority::Normal); return MqttAdmissionResult::Accepted;
    }));
    assert(!pending.drainOne(eligible, [](uint8_t, MqttPublishPriority) { return MqttAdmissionResult::Accepted; }));
}

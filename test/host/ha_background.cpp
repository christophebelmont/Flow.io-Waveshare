#include "Core/Services/IMqtt.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
template<class... Args> void log(Args...) {}
#define LOGE(...) log(__VA_ARGS__)
#define LOGI(...) log(__VA_ARGS__)
#define HA_BOOT_TRACE_D(...) log(__VA_ARGS__)
#define HA_BOOT_TRACE_I(...) log(__VA_ARGS__)
#define pdMS_TO_TICKS(value) (value)
struct DataStore { bool ready = true, published = false; };
bool networkReady(const DataStore& ds) { return ds.ready; }
bool mqttReady(const DataStore& ds) { return ds.ready; }
void setHaAutoconfigPublished(DataStore& ds, bool value) { ds.published = value; }
struct DataStoreService { DataStore* store; };
struct HAModule {
    static constexpr uint8_t ProducerId = 32;
    static constexpr uint16_t HA_PENDING_WORDS = 1, MAX_HA_ENTITIES = 2;
    std::atomic<bool> startupReady_{false}, oneShotCompleted_{false}, refreshRequested_{false};
    bool published_ = false;
    uint16_t admittedMessageId_ = UINT16_MAX;
    uint16_t sensorCount_ = 2, binarySensorCount_ = 0, switchCount_ = 0, numberCount_ = 0,
             buttonCount_ = 0, selectCount_ = 0;
    uint32_t bits[1]{}; uint32_t* pendingBits_ = bits;
    const MqttService* mqttSvc_ = nullptr;
    const DataStoreService* dsSvc_ = nullptr;
    unsigned releases = 0, finishes = 0, bootComplete = 0;
    bool ensureStorage_() { return true; }
    void releaseOneShotResources_() {
        assert(admittedMessageId_ == UINT16_MAX);
        ++releases; oneShotCompleted_ = true;
    }
    void finishLoopTask() { ++finishes; }
    void delayNextCycle(unsigned) {}
    void markBootLogCaptureCompleteAfterDiscovery_() { ++bootComplete; }
    void setStartupReady(bool);
    void requestAutoconfigRefresh();
    void prepareDiscovery_();
    bool enqueuePending_(MqttPublishPriority);
    bool discoveryReady_() const;
    void markAllPending_();
    bool isPending_(uint16_t) const;
    void setPending_(uint16_t,bool);
    bool anyPending_() const;
    uint16_t entityCount_() const;
    void onMessagePublished_(uint16_t);
    void onMessageDropped_(uint16_t);
    void loop();
};
// PRODUCTION_METHODS
int main() {
    DataStore store; DataStoreService data{&store};
    struct Transport { bool accept = false; std::vector<uint16_t> sent; } transport;
    MqttService service{}; service.ctx = &transport;
    service.tryEnqueueBackground = [](void* ctx, uint8_t id, uint16_t message, MqttPublishPriority priority) {
        auto& t = *static_cast<Transport*>(ctx);
        assert(id == 32 && priority == MqttPublishPriority::Low);
        if (!t.accept) return MqttAdmissionResult::Deferred;
        t.sent.push_back(message); return MqttAdmissionResult::Accepted;
    };
    HAModule ha; ha.dsSvc_ = &data; ha.mqttSvc_ = &service;
    ha.requestAutoconfigRefresh(); ha.prepareDiscovery_();
    assert(!ha.enqueuePending_(MqttPublishPriority::Low)); // Startup gate.
    ha.setStartupReady(true);
    for (unsigned i = 0; i < 1000; ++i) {
        ha.prepareDiscovery_();
        assert(!ha.enqueuePending_(MqttPublishPriority::Low));
    }
    assert(ha.anyPending_() && transport.sent.empty());
    transport.accept = true;
    assert(ha.enqueuePending_(MqttPublishPriority::Low));
    assert(transport.sent.back() == 0);
    assert(!ha.enqueuePending_(MqttPublishPriority::Low)); // One admitted discovery entity.
    ha.requestAutoconfigRefresh();
    ha.onMessagePublished_(0);
    ha.prepareDiscovery_();
    assert(ha.isPending_(0)); // Refresh after build survives the old completion.
    assert(ha.enqueuePending_(MqttPublishPriority::Low));
    ha.onMessagePublished_(0);
    assert(!ha.isPending_(0) && ha.isPending_(1));
    assert(ha.enqueuePending_(MqttPublishPriority::Low));
    ha.prepareDiscovery_(); assert(ha.releases == 0); // Never free an admitted builder's tables.
    ha.onMessageDropped_(1);
    assert(!ha.anyPending_() && store.published);
    ha.prepareDiscovery_(); ha.loop();
#if FLOW_HA_ONESHOT_DISCOVERY
    assert(ha.releases == 1 && ha.finishes == 1 && ha.bootComplete == 1);
    assert(!ha.enqueuePending_(MqttPublishPriority::Low));
    ha.requestAutoconfigRefresh(); ha.prepareDiscovery_(); assert(ha.releases == 1);
#else
    assert(ha.releases == 0 && ha.finishes == 0 && ha.bootComplete == 1);
    ha.requestAutoconfigRefresh();
    assert(!ha.anyPending_()); // Public service only signals the MQTT task.
    ha.prepareDiscovery_(); assert(ha.anyPending_() && !store.published);
#endif
}

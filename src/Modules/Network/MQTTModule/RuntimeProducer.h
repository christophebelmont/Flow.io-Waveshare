#pragma once

#include "Core/SystemLimits.h"
#include "Core/RuntimeSnapshotProvider.h"
#include "Core/Services/Services.h"
#include <atomic>

class RuntimeProducer {
public:
    static constexpr uint8_t ProducerId = 4;
    using InitialFullSnapshotCallback = void (*)(void* ctx);

    struct DataChangeStats {
        uint16_t routeCount = 0;
        uint16_t checked = 0;
        uint16_t matched = 0;
        uint16_t enqueueOk = 0;
        uint16_t enqueueFail = 0;
        uint16_t pendingAlready = 0;
        uint32_t enqueueUs = 0;
    };

    void configure(const MqttService* mqttSvc,
                   InitialFullSnapshotCallback initialFullSnapshotCb = nullptr,
                   void* initialFullSnapshotCtx = nullptr);
    bool registerProvider(const IRuntimeSnapshotProvider* provider);
    void rebuildRoutes();
    uint16_t routeCount() const { return routeCount_; }

    void onConnected();
    /** Called by the MQTT task while connected. Deadlines use successful sends only. */
    void tick(uint32_t nowMs);
    bool offerBackground();
    DataChangeStats onDataChanged(DataKey key);

    MqttBuildResult buildMessage(uint16_t messageId, MqttBuildContext& ctx);
    void onMessagePublished(uint16_t messageId);
    void onMessageDropped(uint16_t messageId);

private:
    static constexpr uint8_t MaxProviders = 8;
    static constexpr uint32_t NumericThrottleMs = 10000U;

    struct Route {
        const IRuntimeSnapshotProvider* provider = nullptr;
        uint16_t snapshotIdx = 0;
        RuntimeRouteClass routeClass = RuntimeRouteClass::NumericThrottled;
        char suffix[Limits::TopicBuf] = {0};
        uint32_t refreshIntervalMs = 0;
        bool admitted = false;
        uint32_t revision = 0;
        uint32_t builtRevision = 0;
        uint32_t forceRevision = 0;
        uint32_t builtForceRevision = 0;
        uint32_t lastPublishedTs = 0;
        uint32_t lastPublishMs = 0;
        uint32_t lastBuiltTs = 0;
        bool pending = true;
        bool force = true;
    };

    const MqttService* mqttSvc_ = nullptr;

    const IRuntimeSnapshotProvider* providers_[MaxProviders] = {};
    uint8_t providerCount_ = 0;

    Route* routes_ = nullptr;
    uint16_t routeCount_ = 0;
    uint16_t backgroundCursor_ = 0;
    std::atomic<bool> fullSyncRequested_{false};
    bool initialFullSnapshotActive_ = false;
    bool* initialFullSnapshotDone_ = nullptr;
    uint16_t initialFullSnapshotRemaining_ = 0;
    InitialFullSnapshotCallback initialFullSnapshotCb_ = nullptr;
    void* initialFullSnapshotCtx_ = nullptr;

    bool ensureStorage_(uint16_t count);
    uint16_t storageCapacity_ = 0;
    void markRoutePending_(uint16_t idx, bool force);
    bool enqueueRoute_(uint16_t idx);
    void beginFullSync_();
    void completeInitialFullSnapshotRoute_(uint16_t idx);
    void notifyInitialFullSnapshotComplete_();
};

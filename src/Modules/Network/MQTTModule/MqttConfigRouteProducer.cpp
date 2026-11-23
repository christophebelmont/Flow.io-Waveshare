#include "Modules/Network/MQTTModule/MqttConfigRouteProducer.h"

#include "Core/ErrorCodes.h"
#include "Core/SystemLimits.h"
#include "Modules/Network/MQTTModule/MQTTRuntime.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <new>
#include <stdio.h>
#include <string.h>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::MQTTModule)
#include "Core/ModuleLog.h"

MqttConfigRouteProducer::~MqttConfigRouteProducer()
{
    if (state_) {
        state_->~RouteState();
        heap_caps_free(state_);
    }
}

bool MqttConfigRouteProducer::allocateRouteState_()
{
    if (state_) return true;
    const bool external = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    const uint32_t caps = (external ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
    void* memory = heap_caps_malloc(sizeof(RouteState), caps);
    if (!memory) {
        LOGE("Config producer state allocation failed bytes=%u", (unsigned)sizeof(RouteState));
        return false;
    }
    state_ = new (memory) RouteState{};
    LOGI("Config producer state ready bytes=%u memory=%s", (unsigned)sizeof(RouteState),
         external ? "psram" : "internal");
    return true;
}

bool MqttConfigRouteProducer::buildRelativeTopic(char* dst,
                                                 size_t dstCap,
                                                 const char* base,
                                                 const char* suffix,
                                                 size_t& outLen)
{
    outLen = 0U;
    if (!dst || dstCap == 0U || !base || base[0] == '\0') return false;

    const char* rel = (suffix) ? suffix : "";
    const size_t baseLen = strlen(base);
    const bool hasSuffix = (rel[0] != '\0');
    const bool needSep = (hasSuffix && baseLen > 0U && base[baseLen - 1U] != '/');

    int w = 0;
    if (!hasSuffix) {
        w = snprintf(dst, dstCap, "%s", base);
    } else if (needSep) {
        w = snprintf(dst, dstCap, "%s/%s", base, rel);
    } else {
        w = snprintf(dst, dstCap, "%s%s", base, rel);
    }
    if (!(w > 0 && (size_t)w < dstCap)) return false;
    outLen = (size_t)w;
    return true;
}

void MqttConfigRouteProducer::configure(void* owner,
                                        uint8_t producerId,
                                        const Route* routes,
                                        uint8_t routeCount,
                                        ServiceRegistry& services)
{
    // Register callbacks only once their bounded storage is available.
    if (!allocateRouteState_()) return;
    owner_ = owner;
    producerId_ = producerId;
    routes_ = routes;
    routeCount_ = (routeCount > MaxRoutes) ? MaxRoutes : routeCount;

    mqttSvc_ = services.get<MqttService>(ServiceId::Mqtt);
    cfgSvc_ = services.get<ConfigStoreService>(ServiceId::ConfigStore);
    dsSvc_ = services.get<DataStoreService>(ServiceId::DataStore);
    const EventBusService* ebSvc = services.get<EventBusService>(ServiceId::EventBus);
    eventBus_ = ebSvc ? ebSvc->bus : nullptr;

    if (!eventsSubscribed_ && eventBus_) {
        eventBus_->subscribe(EventId::ConfigChanged, &MqttConfigRouteProducer::onEventStatic_, this);
        eventsSubscribed_ = true;
    }

    if (!producerRegistered_ && mqttSvc_ && mqttSvc_->registerProducer && routeCount_ > 0U) {
        producer_ = MqttPublishProducer{};
        producer_.producerId = producerId_;
        producer_.ctx = this;
        producer_.buildMessage = MqttConfigRouteProducer::buildStatic_;
        producer_.onMessagePublished = MqttConfigRouteProducer::publishedStatic_;
        producer_.onMessageDropped = MqttConfigRouteProducer::droppedStatic_;
        producer_.onTransportTick = MqttConfigRouteProducer::tickStatic_;
        producer_.onBackgroundTick = MqttConfigRouteProducer::backgroundStatic_;
        producerRegistered_ = mqttSvc_->registerProducer(mqttSvc_->ctx, &producer_);
    }

    // One side of the gate is now true: module config is loaded/configured.
    // The transport task publishes only after the MQTT readiness gate opens.
    configLoaded_ = true;
    requestFullSync(MqttPublishPriority::Low);
}

int8_t MqttConfigRouteProducer::findRouteByMessage_(uint16_t messageId) const
{
    for (uint8_t i = 0; i < routeCount_; ++i) {
        if (routes_[i].messageId == messageId) return (int8_t)i;
    }
    return -1;
}

void MqttConfigRouteProducer::setPending_(uint8_t idx, bool pending)
{
    if (idx >= routeCount_ || idx >= MaxRoutes) return;
    state_->pendingFlags_[idx] = pending;
}

bool MqttConfigRouteProducer::isPending_(uint8_t idx) const
{
    if (idx >= routeCount_ || idx >= MaxRoutes) return false;
    return state_->pendingFlags_[idx];
}

MqttPublishPriority MqttConfigRouteProducer::routePriority_(const Route& route)
{
    if (route.changePriority == (uint8_t)MqttPublishPriority::Low) return MqttPublishPriority::Low;
    if (route.changePriority == (uint8_t)MqttPublishPriority::High) return MqttPublishPriority::High;
    return MqttPublishPriority::Normal;
}

MqttAdmissionResult MqttConfigRouteProducer::enqueueByRoute_(uint8_t idx, MqttPublishPriority priority)
{
    if (idx >= routeCount_) return MqttAdmissionResult::Invalid;
    if (!mqttSvc_ || !mqttSvc_->tryEnqueueBackground) return MqttAdmissionResult::Deferred;
    const auto result = mqttSvc_->tryEnqueueBackground(mqttSvc_->ctx, producerId_, routes_[idx].messageId, priority);
    if (result == MqttAdmissionResult::Invalid) {
        LOGE("Invalid config publication producer=%u message=%u", (unsigned)producerId_,
             (unsigned)routes_[idx].messageId);
    }
    if (result == MqttAdmissionResult::Accepted) setPending_(idx, true);
    return result;
}

void MqttConfigRouteProducer::refreshReadyGateAndMaybeSync_()
{
    if (!dsSvc_ || !dsSvc_->store) {
        mqttReadyLatched_ = false;
        return;
    }

    const bool readyNow = mqttReady(*dsSvc_->store);
    const bool rising = readyNow && !mqttReadyLatched_;
    mqttReadyLatched_ = readyNow;
    if (!readyNow) return;
    if (!producerRegistered_ || !configLoaded_) return;
    if (rising) {
        requestFullSync(MqttPublishPriority::Low);
    }
}

void MqttConfigRouteProducer::requestFullSync(MqttPublishPriority prio)
{
    for (uint8_t i = 0; i < routeCount_; ++i) {
        requestedRoutes_.mark(i, prio);
    }
}

void MqttConfigRouteProducer::onEventStatic_(const Event& e, void* ctx)
{
    MqttConfigRouteProducer* self = static_cast<MqttConfigRouteProducer*>(ctx);
    if (self) self->onEvent_(e);
}

void MqttConfigRouteProducer::onEvent_(const Event& e)
{
    if (e.id == EventId::ConfigChanged) {
        const ConfigChangedPayload* p = (const ConfigChangedPayload*)e.payload;
        if (!p) return;

        if (p->moduleId == ConfigBranchRef::UnknownModule ||
            p->localBranchId == ConfigBranchRef::UnknownLocalBranch) {
            requestFullSync(MqttPublishPriority::Low);
            return;
        }

        for (uint8_t i = 0; i < routeCount_; ++i) {
            const bool moduleMatch =
                (routes_[i].branch.moduleId == ConfigBranchRef::UnknownModule) ||
                (routes_[i].branch.moduleId == p->moduleId);
            if (!moduleMatch) continue;

            const bool localMatch =
                (routes_[i].branch.localBranchId == ConfigBranchRef::UnknownLocalBranch) ||
                (routes_[i].branch.localBranchId == p->localBranchId);
            if (!localMatch) continue;

            const MqttPublishPriority prio = routePriority_(routes_[i]);
            requestedRoutes_.mark(i, prio);
        }
    }
}

MqttBuildResult MqttConfigRouteProducer::buildMessage_(uint16_t messageId, MqttBuildContext& ctx)
{
    const int8_t routeIdx = findRouteByMessage_(messageId);
    if (routeIdx < 0) return MqttBuildResult::NoLongerNeeded;

    const uint8_t idx = (uint8_t)routeIdx;
    if (!isPending_(idx)) return MqttBuildResult::NoLongerNeeded;
    const Route& route = routes_[idx];

    if (route.customBuild) {
        return route.customBuild(owner_, messageId, ctx);
    }

    if (!mqttSvc_ || !mqttSvc_->formatTopic || !cfgSvc_ || !cfgSvc_->toJsonModule) {
        return MqttBuildResult::RetryLater;
    }
    if (!route.moduleName || route.moduleName[0] == '\0') return MqttBuildResult::PermanentError;

    char relativeTopic[Limits::Mqtt::Buffers::DynamicTopic] = {0};
    size_t relativeLen = 0U;
    if (route.topicBase && route.topicBase[0] != '\0') {
        const char* relSuffix = route.topicSuffix ? route.topicSuffix : "";
        if (!buildRelativeTopic(relativeTopic,
                                sizeof(relativeTopic),
                                route.topicBase,
                                relSuffix,
                                relativeLen)) {
            return MqttBuildResult::PermanentError;
        }
    } else {
        const char* topicSuffix = route.topicSuffix ? route.topicSuffix : route.moduleName;
        if (!topicSuffix || topicSuffix[0] == '\0') return MqttBuildResult::PermanentError;
        const int sw = snprintf(relativeTopic, sizeof(relativeTopic), "cfg/%s", topicSuffix);
        if (!(sw > 0 && (size_t)sw < sizeof(relativeTopic))) return MqttBuildResult::PermanentError;
        relativeLen = (size_t)sw;
    }

    (void)relativeLen;
    mqttSvc_->formatTopic(mqttSvc_->ctx, relativeTopic, ctx.topic, ctx.topicCapacity);
    if (ctx.topic[0] == '\0') return MqttBuildResult::PermanentError;

    bool truncated = false;
    const bool any = cfgSvc_->toJsonModule(cfgSvc_->ctx, route.moduleName, ctx.payload, ctx.payloadCapacity, &truncated);
    if (truncated) {
        if (!writeErrorJson(ctx.payload, ctx.payloadCapacity, ErrorCode::CfgTruncated, "cfg")) {
            snprintf(ctx.payload, ctx.payloadCapacity, "{\"ok\":false}");
        }
    } else if (!any) {
        return MqttBuildResult::NoLongerNeeded;
    }

    ctx.topicLen = (uint16_t)strnlen(ctx.topic, ctx.topicCapacity);
    ctx.payloadLen = (uint16_t)strnlen(ctx.payload, ctx.payloadCapacity);
    ctx.qos = 1;
    ctx.retain = true;
    return MqttBuildResult::Ready;
}

void MqttConfigRouteProducer::onMessagePublished_(uint16_t messageId)
{
    const int8_t idx = findRouteByMessage_(messageId);
    if (idx >= 0) setPending_((uint8_t)idx, false);
    // Concurrent configuration events live in requestedRoutes_, independently
    // of this admitted job. Completing it never clears those newer requests.
}

void MqttConfigRouteProducer::onMessageDropped_(uint16_t messageId)
{
    onMessagePublished_(messageId);
}

void MqttConfigRouteProducer::tickStatic_(void* ctx, uint32_t nowMs)
{
    MqttConfigRouteProducer* self = static_cast<MqttConfigRouteProducer*>(ctx);
    if (self) self->onTransportTick_(nowMs);
}

void MqttConfigRouteProducer::onTransportTick_(uint32_t)
{
    refreshReadyGateAndMaybeSync_();
}

bool MqttConfigRouteProducer::backgroundStatic_(void* ctx)
{
    auto* self = static_cast<MqttConfigRouteProducer*>(ctx);
    return self && self->offerBackground_();
}

bool MqttConfigRouteProducer::offerBackground_()
{
    if (!mqttReadyLatched_ || !producerRegistered_ || !configLoaded_) return false;
    return requestedRoutes_.drainOne(
        [this](uint8_t idx) { return idx < routeCount_ && !isPending_(idx); },
        [this](uint8_t idx, MqttPublishPriority priority) { return enqueueByRoute_(idx, priority); });
}

MqttBuildResult MqttConfigRouteProducer::buildStatic_(void* ctx, uint16_t messageId, MqttBuildContext& buildCtx)
{
    MqttConfigRouteProducer* self = static_cast<MqttConfigRouteProducer*>(ctx);
    return self ? self->buildMessage_(messageId, buildCtx) : MqttBuildResult::PermanentError;
}

void MqttConfigRouteProducer::publishedStatic_(void* ctx, uint16_t messageId)
{
    MqttConfigRouteProducer* self = static_cast<MqttConfigRouteProducer*>(ctx);
    if (self) self->onMessagePublished_(messageId);
}

void MqttConfigRouteProducer::droppedStatic_(void* ctx, uint16_t messageId)
{
    MqttConfigRouteProducer* self = static_cast<MqttConfigRouteProducer*>(ctx);
    if (self) self->onMessageDropped_(messageId);
}

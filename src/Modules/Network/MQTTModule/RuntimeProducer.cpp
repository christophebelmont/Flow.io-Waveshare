#include "Modules/Network/MQTTModule/RuntimeProducer.h"
#include "Core/PerformanceDiagnostics.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <new>
#include <string.h>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::MQTTModule)
#include "Core/ModuleLog.h"

namespace {
template <typename T>
T* allocPsramArray_(size_t count)
{
    void* mem = heap_caps_malloc(sizeof(T) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!mem) return nullptr;

    T* out = static_cast<T*>(mem);
    for (size_t i = 0; i < count; ++i) {
        new (&out[i]) T();
    }
    return out;
}

bool* allocPsramBoolArray_(size_t count)
{
    void* mem = heap_caps_calloc(count, sizeof(bool), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    return static_cast<bool*>(mem);
}
} // namespace

bool RuntimeProducer::ensureStorage_(uint16_t count)
{
    if (count <= storageCapacity_) return true;
    auto* routes = allocPsramArray_<Route>(count);
    auto* done = allocPsramBoolArray_(count);
    if (!routes || !done) { heap_caps_free(routes); heap_caps_free(done); return false; }
    heap_caps_free(routes_); heap_caps_free(initialFullSnapshotDone_);
    routes_ = routes; initialFullSnapshotDone_ = done; storageCapacity_ = count;
    return true;
}

void RuntimeProducer::configure(const MqttService* mqttSvc,
                                InitialFullSnapshotCallback initialFullSnapshotCb,
                                void* initialFullSnapshotCtx)
{
    mqttSvc_ = mqttSvc;
    initialFullSnapshotCb_ = initialFullSnapshotCb;
    initialFullSnapshotCtx_ = initialFullSnapshotCtx;
}

bool RuntimeProducer::registerProvider(const IRuntimeSnapshotProvider* provider)
{
    if (!provider) return false;
    if (providerCount_ >= MaxProviders) return false;

    for (uint8_t i = 0; i < providerCount_; ++i) {
        if (providers_[i] == provider) return true;
    }

    providers_[providerCount_++] = provider;
    rebuildRoutes();
    return true;
}

void RuntimeProducer::rebuildRoutes()
{
    uint16_t count = 0;
    for (uint8_t p = 0; p < providerCount_; ++p) count += providers_[p]->runtimeSnapshotCount();
    if (count > Limits::MaxRuntimeRoutes || !ensureStorage_(count)) {
        LOGE("Runtime route allocation failed count=%u", unsigned(count));
        routeCount_ = 0;
        return;
    }
    routeCount_ = 0;

    for (uint8_t p = 0; p < providerCount_; ++p) {
        const IRuntimeSnapshotProvider* provider = providers_[p];
        if (!provider) continue;

        const uint16_t count = provider->runtimeSnapshotCount();
        for (uint16_t i = 0; i < count; ++i) {

            const char* suffix = provider->runtimeSnapshotSuffix(i);
            if (!suffix || suffix[0] == '\0') continue;

            Route& route = routes_[routeCount_++];
            route = Route{};
            route.provider = provider;
            route.snapshotIdx = i;
            route.routeClass = provider->runtimeSnapshotClass(i);
            route.refreshIntervalMs = provider->runtimeSnapshotRefreshMs(i);
            route.pending = true;
            route.force = true;
            snprintf(route.suffix, sizeof(route.suffix), "%s", suffix);

        }
    }
}

void RuntimeProducer::markRoutePending_(uint16_t idx, bool force)
{
    if (idx >= routeCount_) return;
    Route& route = routes_[idx];
    route.pending = true;
    route.force = route.force || force;
    ++route.revision;
    if (force) route.forceRevision = route.revision;
}

bool RuntimeProducer::enqueueRoute_(uint16_t idx)
{
    if (!mqttSvc_ || idx >= routeCount_) return false;
    Route& route = routes_[idx];
    if (route.admitted) return true;
    bool accepted = false;
    if (route.routeClass == RuntimeRouteClass::ActuatorImmediate) {
        accepted = mqttSvc_->enqueue && mqttSvc_->enqueue(mqttSvc_->ctx, ProducerId, idx,
                                                          (uint8_t)MqttPublishPriority::High, 0);
    } else if (mqttSvc_->tryEnqueueBackground) {
        const auto result = mqttSvc_->tryEnqueueBackground(mqttSvc_->ctx, ProducerId, idx, MqttPublishPriority::Normal);
        if (result == MqttAdmissionResult::Invalid) {
            LOGE("Invalid runtime publication message=%u", (unsigned)idx);
            route.builtRevision = route.revision;
            onMessageDropped(idx);
            return false;
        }
        accepted = result == MqttAdmissionResult::Accepted;
    }
    route.admitted = accepted;
    return accepted;
}

bool RuntimeProducer::offerBackground()
{
    for (uint16_t n = 0; n < routeCount_; ++n) {
        const uint16_t idx = (uint16_t)((backgroundCursor_ + n) % routeCount_);
        const Route& route = routes_[idx];
        if (!route.provider || !route.pending || route.admitted ||
            route.routeClass == RuntimeRouteClass::ActuatorImmediate) continue;
        if (!route.force && route.routeClass == RuntimeRouteClass::NumericThrottled &&
            (uint32_t)(millis() - route.lastPublishMs) < NumericThrottleMs) continue;
        backgroundCursor_ = (uint16_t)((idx + 1U) % routeCount_);
        return enqueueRoute_(idx);
    }
    return false;
}

void RuntimeProducer::completeInitialFullSnapshotRoute_(uint16_t idx)
{
    if (!initialFullSnapshotActive_) return;
    if (idx >= routeCount_) return;
    if (initialFullSnapshotDone_[idx]) return;

    initialFullSnapshotDone_[idx] = true;
    if (initialFullSnapshotRemaining_ > 0U) {
        --initialFullSnapshotRemaining_;
    }
    if (initialFullSnapshotRemaining_ == 0U) {
        notifyInitialFullSnapshotComplete_();
    }
}

void RuntimeProducer::notifyInitialFullSnapshotComplete_()
{
    initialFullSnapshotActive_ = false;
    if (initialFullSnapshotCb_) {
        initialFullSnapshotCb_(initialFullSnapshotCtx_);
    }
}

void RuntimeProducer::onConnected()
{
    // The ESP-MQTT client task only signals; route state belongs to the MQTT task.
    fullSyncRequested_.store(true);
}

void RuntimeProducer::beginFullSync_()
{
    if (!ensureStorage_(routeCount_)) return;
    if (initialFullSnapshotDone_) memset(initialFullSnapshotDone_, 0, sizeof(bool) * storageCapacity_);
    initialFullSnapshotRemaining_ = routeCount_;
    initialFullSnapshotActive_ = true;
    if (routeCount_ == 0U) {
        notifyInitialFullSnapshotComplete_();
        return;
    }

    for (uint16_t i = 0; i < routeCount_; ++i) {
        const Route& route = routes_[i];
        LOGD("route producer=%u type=1 msg=%u snapshot=%u class=%u suffix=%s",
             (unsigned)ProducerId, (unsigned)i, (unsigned)route.snapshotIdx,
             (unsigned)route.routeClass, route.suffix);
        markRoutePending_(i, true);
    }
}

void RuntimeProducer::tick(uint32_t nowMs)
{
    if (!mqttSvc_ || !mqttSvc_->isConnected || !mqttSvc_->isConnected(mqttSvc_->ctx)) return;
    if (fullSyncRequested_.exchange(false)) beginFullSync_();
    for (uint16_t i = 0; i < routeCount_; ++i) {
        Route& route = routes_[i];
        if (!route.provider) continue;
        if (route.refreshIntervalMs && !route.pending && !route.admitted &&
            (uint32_t)(nowMs - route.lastPublishMs) >= route.refreshIntervalMs) {
            markRoutePending_(i, true);
        }
        if (route.pending && !route.admitted && route.routeClass == RuntimeRouteClass::ActuatorImmediate)
            (void)enqueueRoute_(i);
    }
}

RuntimeProducer::DataChangeStats RuntimeProducer::onDataChanged(DataKey key)
{
    DataChangeStats stats{};
    stats.routeCount = routeCount_;
    for (uint16_t i = 0; i < routeCount_; ++i) {
        Route& route = routes_[i];
        if (!route.provider) continue;
        ++stats.checked;
        if (!route.provider->runtimeSnapshotAffectsKey(route.snapshotIdx, key)) continue;
        ++stats.matched;
        if (route.pending || route.force) ++stats.pendingAlready;
        markRoutePending_(i, false);
#if FLOW_PERFORMANCE_DIAGNOSTICS
        const uint32_t t0 = micros();
#endif
        const bool ok = route.routeClass == RuntimeRouteClass::ActuatorImmediate && enqueueRoute_(i);
#if FLOW_PERFORMANCE_DIAGNOSTICS
        stats.enqueueUs += (uint32_t)(micros() - t0);
#endif
        if (ok) {
            ++stats.enqueueOk;
        } else if (route.routeClass == RuntimeRouteClass::ActuatorImmediate) {
            ++stats.enqueueFail;
        }
    }
    return stats;
}

MqttBuildResult RuntimeProducer::buildMessage(uint16_t messageId, MqttBuildContext& ctx)
{
    if (messageId >= routeCount_) return MqttBuildResult::NoLongerNeeded;

    Route& route = routes_[messageId];
    route.builtRevision = route.revision;
    route.builtForceRevision = route.forceRevision;
    if (!route.provider) return MqttBuildResult::NoLongerNeeded;
    if (!route.pending && !route.force) return MqttBuildResult::NoLongerNeeded;
    if (!route.provider->runtimeSnapshotSuffix(route.snapshotIdx)) {
        route.pending = false;
        route.force = false;
        return MqttBuildResult::NoLongerNeeded;
    }

    const uint32_t nowMs = millis();
    if (!route.force && route.routeClass == RuntimeRouteClass::NumericThrottled) {
        if ((uint32_t)(nowMs - route.lastPublishMs) < NumericThrottleMs) {
            return MqttBuildResult::RetryLater;
        }
    }

    uint32_t ts = 0;
    if (!route.provider->buildRuntimeSnapshot(route.snapshotIdx, ctx.payload, ctx.payloadCapacity, ts)) {
        return MqttBuildResult::RetryLater;
    }

    const uint32_t effectiveTs = (ts == 0U) ? 1U : ts;
    if (!route.force && route.lastPublishedTs != 0U && effectiveTs <= route.lastPublishedTs) {
        return MqttBuildResult::NoLongerNeeded;
    }

    if (!mqttSvc_ || !mqttSvc_->formatTopic) return MqttBuildResult::PermanentError;
    mqttSvc_->formatTopic(mqttSvc_->ctx, route.suffix, ctx.topic, ctx.topicCapacity);
    if (ctx.topic[0] == '\0') return MqttBuildResult::PermanentError;

    ctx.topicLen = (uint16_t)strnlen(ctx.topic, ctx.topicCapacity);
    ctx.payloadLen = (uint16_t)strnlen(ctx.payload, ctx.payloadCapacity);
    ctx.qos = 0;
    ctx.retain = false;

    route.lastBuiltTs = effectiveTs;
    return MqttBuildResult::Ready;
}

void RuntimeProducer::onMessagePublished(uint16_t messageId)
{
    if (messageId >= routeCount_) return;

    Route& route = routes_[messageId];
    route.lastPublishedTs = route.lastBuiltTs;
    route.lastPublishMs = millis();
    route.admitted = false;
    if (route.forceRevision == route.builtForceRevision) route.force = false;
    if (route.revision == route.builtRevision) {
        route.pending = false;
        route.force = false;
    }
    completeInitialFullSnapshotRoute_(messageId);
}

void RuntimeProducer::onMessageDropped(uint16_t messageId)
{
    if (messageId >= routeCount_) return;
    Route& route = routes_[messageId];
    route.admitted = false;
    if (route.revision == route.builtRevision) {
        route.pending = false;
        route.force = false;
    }
    completeInitialFullSnapshotRoute_(messageId);
}

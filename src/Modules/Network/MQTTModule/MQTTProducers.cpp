/**
 * @file MQTTProducers.cpp
 * @brief Runtime publishers, alarm sync and producer message builders for MQTTModule.
 */

#include "MQTTModule.h"

#include "Core/BufferUsageTracker.h"
#include <string.h>

bool MQTTModule::registerRuntimeProvider(const IRuntimeSnapshotProvider* provider)
{
    return runtimeProducerCore_.registerProvider(provider);
}

bool MQTTModule::addRuntimePublisher(const char* topic,
                                     uint32_t periodMs,
                                     int qos,
                                     bool retain,
                                     bool (*build)(MQTTModule* self, char* out, size_t outLen),
                                     bool allowNoPayload)
{
    if (!topic || !build) return false;
    if (runtimePublisherCount_ >= Limits::Mqtt::Capacity::MaxPublishers) return false;

    RuntimePublisher& p = runtimePublishers_[runtimePublisherCount_];
    p.used = true;
    p.messageId = (uint16_t)(StatusMsgRuntimeBase + runtimePublisherCount_);
    p.topic = topic;
    p.periodMs = periodMs;
    p.nextDueMs = 0;
    p.qos = (qos < 0) ? 0U : (uint8_t)qos;
    p.retain = retain;
    p.build = build;
    p.allowNoPayload = allowNoPayload;
    ++runtimePublisherCount_;
    return true;
}

void MQTTModule::tickProducers_(uint32_t nowMs)
{
    const MqttPublishProducer* snapshot[MaxProducers]{};
    uint8_t count = 0;

    portENTER_CRITICAL(&producerMux_);
    count = producerCount_;
    if (count > MaxProducers) count = MaxProducers;
    for (uint8_t i = 0; i < count; ++i) {
        snapshot[i] = producers_[i];
    }
    portEXIT_CRITICAL(&producerMux_);

    for (uint8_t i = 0; i < count; ++i) {
        const MqttPublishProducer* producer = snapshot[i];
        if (!producer || !producer->onTransportTick) continue;
        producer->onTransportTick(producer->ctx, nowMs);
    }
}

bool MQTTModule::enqueueAck_(const char* topicSuffix,
                             const char* payload,
                             uint8_t qos,
                             bool retain,
                             MqttPublishPriority priority)
{
    if (!topicSuffix || topicSuffix[0] == '\0') return false;
    if (!payload) return false;

    AckMessage& m = ackMessages_[ackWriteCursor_];
    ackWriteCursor_ = (uint8_t)((ackWriteCursor_ + 1U) % MaxAckMessages);

    m = AckMessage{};
    m.used = true;
    m.messageId = ackNextMessageId_;
    m.qos = qos;
    m.retain = retain;
    snprintf(m.topicSuffix, sizeof(m.topicSuffix), "%s", topicSuffix);
    snprintf(m.payload, sizeof(m.payload), "%s", payload);

    uint8_t ackUsedCount = 0U;
    for (uint8_t i = 0; i < MaxAckMessages; ++i) {
        if (ackMessages_[i].used) ++ackUsedCount;
    }
    BufferUsageTracker::note(TrackedBufferId::MqttAckMessages,
                             (size_t)ackUsedCount * sizeof(AckMessage),
                             sizeof(ackMessages_),
                             topicSuffix,
                             nullptr);

    ++ackNextMessageId_;
    if (ackNextMessageId_ == 0U) ++ackNextMessageId_;

    if (!enqueue(ProducerIdAck, m.messageId, priority, 0)) {
        m.used = false;
        return false;
    }
    return true;
}

void MQTTModule::enqueueAlarmFullSync_()
{
    if (!alarmSvc_ || !alarmSvc_->listIds) return;

    AlarmId ids[Limits::Alarm::MaxAlarms]{};
    const uint8_t n = alarmSvc_->listIds(alarmSvc_->ctx, ids, (uint8_t)Limits::Alarm::MaxAlarms);
    for (uint8_t i = 0; i < n; ++i) {
        const uint16_t msg = (uint16_t)(AlarmMsgStateBase + (uint16_t)ids[i]);
        (void)enqueue(ProducerIdAlarm, msg, MqttPublishPriority::High, 0);
    }
    (void)enqueue(ProducerIdAlarm, AlarmMsgMeta, MqttPublishPriority::Normal, 0);
    (void)enqueue(ProducerIdAlarm, AlarmMsgPack, MqttPublishPriority::Normal, 0);
}

MqttBuildResult MQTTModule::buildAckStatic_(void* ctx, uint16_t messageId, MqttBuildContext& buildCtx)
{
    MQTTModule* self = static_cast<MQTTModule*>(ctx);
    return self ? self->buildAck_(messageId, buildCtx) : MqttBuildResult::PermanentError;
}

void MQTTModule::onAckPublishedStatic_(void* ctx, uint16_t messageId)
{
    MQTTModule* self = static_cast<MQTTModule*>(ctx);
    if (self) self->onAckPublished_(messageId);
}

void MQTTModule::onAckDroppedStatic_(void* ctx, uint16_t messageId)
{
    MQTTModule* self = static_cast<MQTTModule*>(ctx);
    if (self) self->onAckDropped_(messageId);
}

MqttBuildResult MQTTModule::buildStatusStatic_(void* ctx, uint16_t messageId, MqttBuildContext& buildCtx)
{
    MQTTModule* self = static_cast<MQTTModule*>(ctx);
    return self ? self->buildStatus_(messageId, buildCtx) : MqttBuildResult::PermanentError;
}

MqttBuildResult MQTTModule::buildAlarmStatic_(void* ctx, uint16_t messageId, MqttBuildContext& buildCtx)
{
    MQTTModule* self = static_cast<MQTTModule*>(ctx);
    return self ? self->buildAlarm_(messageId, buildCtx) : MqttBuildResult::PermanentError;
}

MqttBuildResult MQTTModule::buildAck_(uint16_t messageId, MqttBuildContext& ctx)
{
    for (uint8_t i = 0; i < MaxAckMessages; ++i) {
        const AckMessage& msg = ackMessages_[i];
        if (!msg.used || msg.messageId != messageId) continue;

        const int tw = snprintf(ctx.topic, ctx.topicCapacity, "%s/%s/%s", cfgData_.baseTopic, deviceId_, msg.topicSuffix);
        if (!(tw > 0 && (uint16_t)tw < ctx.topicCapacity)) return MqttBuildResult::PermanentError;

        const int pw = snprintf(ctx.payload, ctx.payloadCapacity, "%s", msg.payload);
        if (!(pw > 0 && (uint16_t)pw < ctx.payloadCapacity)) return MqttBuildResult::PermanentError;

        ctx.topicLen = (uint16_t)tw;
        ctx.payloadLen = (uint16_t)pw;
        ctx.qos = msg.qos;
        ctx.retain = msg.retain;
        return MqttBuildResult::Ready;
    }
    return MqttBuildResult::NoLongerNeeded;
}

void MQTTModule::onAckPublished_(uint16_t messageId)
{
    for (uint8_t i = 0; i < MaxAckMessages; ++i) {
        AckMessage& msg = ackMessages_[i];
        if (msg.used && msg.messageId == messageId) {
            msg.used = false;
            uint8_t ackUsedCount = 0U;
            for (uint8_t j = 0; j < MaxAckMessages; ++j) {
                if (ackMessages_[j].used) ++ackUsedCount;
            }
            BufferUsageTracker::note(TrackedBufferId::MqttAckMessages,
                                     (size_t)ackUsedCount * sizeof(AckMessage),
                                     sizeof(ackMessages_),
                                     "ack_free",
                                     nullptr);
            break;
        }
    }
}

void MQTTModule::onAckDropped_(uint16_t messageId)
{
    onAckPublished_(messageId);
}

MqttBuildResult MQTTModule::buildStatus_(uint16_t messageId, MqttBuildContext& ctx)
{
    if (messageId == StatusMsgOnline) {
        const int tw = snprintf(ctx.topic, ctx.topicCapacity, "%s", topicStatus_);
        if (!(tw > 0 && (uint16_t)tw < ctx.topicCapacity)) return MqttBuildResult::PermanentError;

        static const char kOnlinePayload[] = "{\"online\":true}";
        const int pw = snprintf(ctx.payload, ctx.payloadCapacity, "%s", kOnlinePayload);
        if (!(pw > 0 && (uint16_t)pw < ctx.payloadCapacity)) return MqttBuildResult::PermanentError;

        ctx.topicLen = (uint16_t)tw;
        ctx.payloadLen = (uint16_t)pw;
        ctx.qos = 1;
        ctx.retain = true;
        return MqttBuildResult::Ready;
    }

    if (messageId >= StatusMsgRuntimeBase) {
        const uint8_t idx = (uint8_t)(messageId - StatusMsgRuntimeBase);
        if (idx >= runtimePublisherCount_) return MqttBuildResult::NoLongerNeeded;

        RuntimePublisher& p = runtimePublishers_[idx];
        if (!p.used || !p.topic || !p.build) return MqttBuildResult::NoLongerNeeded;

        const int tw = snprintf(ctx.topic, ctx.topicCapacity, "%s", p.topic);
        if (!(tw > 0 && (uint16_t)tw < ctx.topicCapacity)) return MqttBuildResult::PermanentError;

        const bool ok = p.build(this, ctx.payload, ctx.payloadCapacity);
        if (!ok) {
            return p.allowNoPayload ? MqttBuildResult::NoLongerNeeded : MqttBuildResult::RetryLater;
        }

        ctx.topicLen = (uint16_t)tw;
        ctx.payloadLen = (uint16_t)strnlen(ctx.payload, ctx.payloadCapacity);
        ctx.qos = p.qos;
        ctx.retain = p.retain;
        return MqttBuildResult::Ready;
    }

    return MqttBuildResult::NoLongerNeeded;
}

MqttBuildResult MQTTModule::buildAlarm_(uint16_t messageId, MqttBuildContext& ctx)
{
    if (!alarmSvc_) return MqttBuildResult::RetryLater;

    if (messageId == AlarmMsgMeta) {
        if (!alarmSvc_->activeCount || !alarmSvc_->highestSeverity) return MqttBuildResult::PermanentError;

        const int tw = snprintf(ctx.topic, ctx.topicCapacity, "%s/%s/rt/alarms/m", cfgData_.baseTopic, deviceId_);
        if (!(tw > 0 && (uint16_t)tw < ctx.topicCapacity)) return MqttBuildResult::PermanentError;

        const uint8_t active = alarmSvc_->activeCount(alarmSvc_->ctx);
        const AlarmSeverity highest = alarmSvc_->highestSeverity(alarmSvc_->ctx);
        const int pw = snprintf(ctx.payload,
                                ctx.payloadCapacity,
                                "{\"a\":%u,\"h\":%u,\"ts\":%lu}",
                                (unsigned)active,
                                (unsigned)((uint8_t)highest),
                                (unsigned long)millis());
        if (!(pw > 0 && (uint16_t)pw < ctx.payloadCapacity)) return MqttBuildResult::PermanentError;

        ctx.topicLen = (uint16_t)tw;
        ctx.payloadLen = (uint16_t)pw;
        ctx.qos = 0;
        ctx.retain = false;
        return MqttBuildResult::Ready;
    }

    if (messageId == AlarmMsgPack) {
        if (!alarmSvc_->buildPacked) return MqttBuildResult::PermanentError;

        const int tw = snprintf(ctx.topic, ctx.topicCapacity, "%s/%s/rt/alarms/p", cfgData_.baseTopic, deviceId_);
        if (!(tw > 0 && (uint16_t)tw < ctx.topicCapacity)) return MqttBuildResult::PermanentError;

        if (!alarmSvc_->buildPacked(alarmSvc_->ctx, ctx.payload, ctx.payloadCapacity, 8)) {
            return MqttBuildResult::RetryLater;
        }

        ctx.topicLen = (uint16_t)tw;
        ctx.payloadLen = (uint16_t)strnlen(ctx.payload, ctx.payloadCapacity);
        ctx.qos = 0;
        ctx.retain = false;
        return MqttBuildResult::Ready;
    }

    if (messageId >= AlarmMsgStateBase) {
        if (!alarmSvc_->buildAlarmState) return MqttBuildResult::PermanentError;

        const AlarmId id = (AlarmId)(messageId - AlarmMsgStateBase);

        const int tw = snprintf(ctx.topic,
                                ctx.topicCapacity,
                                "%s/%s/rt/alarms/id%u",
                                cfgData_.baseTopic,
                                deviceId_,
                                (unsigned)((uint16_t)id));
        if (!(tw > 0 && (uint16_t)tw < ctx.topicCapacity)) return MqttBuildResult::PermanentError;

        if (!alarmSvc_->buildAlarmState(alarmSvc_->ctx, id, ctx.payload, ctx.payloadCapacity)) {
            return MqttBuildResult::RetryLater;
        }

        ctx.topicLen = (uint16_t)tw;
        ctx.payloadLen = (uint16_t)strnlen(ctx.payload, ctx.payloadCapacity);
        ctx.qos = 0;
        ctx.retain = false;
        return MqttBuildResult::Ready;
    }

    return MqttBuildResult::NoLongerNeeded;
}

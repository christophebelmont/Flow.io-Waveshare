/**
 * @file MQTTQueue.cpp
 * @brief Producer registry, job queues and publish dispatch for MQTTModule.
 */

#include "MQTTModule.h"

#include "Core/BufferUsageTracker.h"

#include <esp_heap_caps.h>
#include <string.h>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::MQTTModule)
#include "Core/ModuleLog.h"

bool MQTTModule::registerProducer(const MqttPublishProducer* producer)
{
    if (!producer) return false;
    if (producer->producerId == 0U) return false;
    if (!producer->buildMessage) return false;

    bool ok = false;
    portENTER_CRITICAL(&producerMux_);
    for (uint8_t i = 0; i < producerCount_; ++i) {
        if (producers_[i] && producers_[i]->producerId == producer->producerId) {
            producers_[i] = producer;
            ok = true;
            break;
        }
    }

    if (!ok) {
        if (producerCount_ < MaxProducers) {
            producers_[producerCount_++] = producer;
            ok = true;
        }
    }
    portEXIT_CRITICAL(&producerMux_);
    return ok;
}

bool MQTTModule::registerInboundHandler(const MqttInboundHandler* handler)
{
    if (!handler || !handler->topicSuffix || handler->topicSuffix[0] == '\0' || !handler->onMessage) {
        return false;
    }

    bool ok = false;
    portENTER_CRITICAL(&inboundMux_);
    for (uint8_t i = 0; i < inboundHandlerCount_; ++i) {
        const MqttInboundHandler* h = inboundHandlers_[i];
        if (h && h->topicSuffix && strcmp(h->topicSuffix, handler->topicSuffix) == 0) {
            inboundHandlers_[i] = handler;
            ok = true;
            break;
        }
    }
    if (!ok && inboundHandlerCount_ < MaxInboundHandlers) {
        inboundHandlers_[inboundHandlerCount_++] = handler;
        ok = true;
    }
    portEXIT_CRITICAL(&inboundMux_);
    return ok;
}

const MqttPublishProducer* MQTTModule::findProducer_(uint8_t producerId) const
{
    const MqttPublishProducer* out = nullptr;
    portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&producerMux_));
    for (uint8_t i = 0; i < producerCount_; ++i) {
        const MqttPublishProducer* p = producers_[i];
        if (p && p->producerId == producerId) {
            out = p;
            break;
        }
    }
    portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&producerMux_));
    return out;
}

int16_t MQTTModule::findJobSlot_(uint8_t producerId, uint16_t messageId) const
{
    for (uint8_t i = 0; i < MaxJobs; ++i) {
        const Job& job = jobs_[i];
        if (!job.used) continue;
        if (job.producerId == producerId && job.messageId == messageId) return (int16_t)i;
    }
    return -1;
}

int16_t MQTTModule::allocJobSlot_()
{
    for (uint8_t i = 0; i < MaxJobs; ++i) {
        if (!jobs_[i].used) return (int16_t)i;
    }
    return -1;
}

bool MQTTModule::queuePush_(uint8_t prio, const JobQueueItem& item)
{
    if (prio == (uint8_t)MqttPublishPriority::High) {
        if (highQ_.count >= HighQueueCap) return false;
        highQ_.items[highQ_.tail] = item;
        highQ_.tail = (uint16_t)((highQ_.tail + 1U) % HighQueueCap);
        ++highQ_.count;
        return true;
    }
    if (prio == (uint8_t)MqttPublishPriority::Normal) {
        if (normalQ_.count >= NormalQueueCap) return false;
        normalQ_.items[normalQ_.tail] = item;
        normalQ_.tail = (uint16_t)((normalQ_.tail + 1U) % NormalQueueCap);
        ++normalQ_.count;
        return true;
    }
    if (lowQ_.count >= LowQueueCap) return false;
    lowQ_.items[lowQ_.tail] = item;
    lowQ_.tail = (uint16_t)((lowQ_.tail + 1U) % LowQueueCap);
    ++lowQ_.count;
    return true;
}

bool MQTTModule::queuePop_(uint8_t prio, JobQueueItem& out)
{
    if (prio == (uint8_t)MqttPublishPriority::High) {
        if (highQ_.count == 0U) return false;
        out = highQ_.items[highQ_.head];
        highQ_.head = (uint16_t)((highQ_.head + 1U) % HighQueueCap);
        --highQ_.count;
        return true;
    }
    if (prio == (uint8_t)MqttPublishPriority::Normal) {
        if (normalQ_.count == 0U) return false;
        out = normalQ_.items[normalQ_.head];
        normalQ_.head = (uint16_t)((normalQ_.head + 1U) % NormalQueueCap);
        --normalQ_.count;
        return true;
    }
    if (lowQ_.count == 0U) return false;
    out = lowQ_.items[lowQ_.head];
    lowQ_.head = (uint16_t)((lowQ_.head + 1U) % LowQueueCap);
    --lowQ_.count;
    return true;
}

bool MQTTModule::queueSlot_(uint8_t slotIdx, uint8_t prio, bool invalidateOld)
{
    if (slotIdx >= MaxJobs) return false;
    Job& job = jobs_[slotIdx];
    if (!job.used) return false;

    if (invalidateOld && job.queued) {
        ++job.queueToken;
        job.queued = false;
    }

    if (job.queued) return true;

    job.queuedPrio = prio;
    job.queued = true;
    ++job.queueToken;

    JobQueueItem item{};
    item.slot = slotIdx;
    item.token = job.queueToken;

    if (!queuePush_(prio, item)) {
        job.queued = false;
        return false;
    }

    return true;
}

void MQTTModule::snapshotQueueStatsNoLock_(uint16_t& jobsUsed,
                                           uint16_t& highCount,
                                           uint16_t& normalCount,
                                           uint16_t& lowCount) const
{
    jobsUsed = 0U;
    for (uint8_t i = 0; i < MaxJobs; ++i) {
        if (jobs_[i].used) ++jobsUsed;
    }
    highCount = highQ_.count;
    normalCount = normalQ_.count;
    lowCount = lowQ_.count;
}

void MQTTModule::logEnqueueReject_(uint8_t producerId,
                                   uint16_t messageId,
                                   uint8_t priority,
                                   const char* reason,
                                   uint16_t jobsUsed,
                                   uint16_t highCount,
                                   uint16_t normalCount,
                                   uint16_t lowCount)
{
    const uint32_t nowMs = millis();
    static constexpr uint32_t kMinLogIntervalMs = 1000U;
    if ((uint32_t)(nowMs - lastEnqueueRejectLogMs_) < kMinLogIntervalMs) return;
    lastEnqueueRejectLogMs_ = nowMs;

    LOGW("enqueue reject reason=%s producer=%u msg=%u prio=%u jobs=%u/%u q(h=%u/%u,n=%u/%u,l=%u/%u)",
         reason ? reason : "unknown",
         (unsigned)producerId,
         (unsigned)messageId,
         (unsigned)priority,
         (unsigned)jobsUsed,
         (unsigned)MaxJobs,
         (unsigned)highCount,
         (unsigned)HighQueueCap,
         (unsigned)normalCount,
         (unsigned)NormalQueueCap,
         (unsigned)lowCount,
         (unsigned)LowQueueCap);
}

bool MQTTModule::enqueueJob_(uint8_t producerId, uint16_t messageId, uint8_t priority, uint8_t flags)
{
    bool ok = false;
    bool shouldLogReject = false;
    const bool silentRejectLog = (flags & (uint8_t)MqttEnqueueFlags::SilentRejectLog) != 0U;
    const char* rejectReason = nullptr;
    uint16_t jobsUsed = 0U;
    uint16_t highCount = 0U;
    uint16_t normalCount = 0U;
    uint16_t lowCount = 0U;
    portENTER_CRITICAL(&jobsMux_);

    int16_t idx = findJobSlot_(producerId, messageId);
    if (idx >= 0) {
        Job& job = jobs_[(uint8_t)idx];
        job.flags |= flags;

        if (priority > job.priority) {
            job.priority = priority;
        }

        if (job.processing) {
            job.requeueAfterProcess = true;
            ok = true;
            portEXIT_CRITICAL(&jobsMux_);
            return ok;
        }

        if (job.queued) {
            if (job.priority > job.queuedPrio) {
                ok = queueSlot_((uint8_t)idx, job.priority, true);
                if (!ok) {
                    rejectReason = "queue_full";
                    snapshotQueueStatsNoLock_(jobsUsed, highCount, normalCount, lowCount);
                    shouldLogReject = !silentRejectLog;
                }
            } else {
                ok = true;
            }
            portEXIT_CRITICAL(&jobsMux_);
            return ok;
        }

        job.retryCount = 0;
        job.notBeforeMs = 0;
        ok = queueSlot_((uint8_t)idx, job.priority, false);
        if (!ok) {
            rejectReason = "queue_full";
            snapshotQueueStatsNoLock_(jobsUsed, highCount, normalCount, lowCount);
            shouldLogReject = !silentRejectLog;
        }
        portEXIT_CRITICAL(&jobsMux_);
        if (shouldLogReject) {
            logEnqueueReject_(producerId, messageId, priority, rejectReason, jobsUsed, highCount, normalCount, lowCount);
        }
        return ok;
    }

    idx = allocJobSlot_();
    if (idx < 0) {
        rejectReason = "slot_full";
        snapshotQueueStatsNoLock_(jobsUsed, highCount, normalCount, lowCount);
        shouldLogReject = !silentRejectLog;
        portEXIT_CRITICAL(&jobsMux_);
        if (shouldLogReject) {
            logEnqueueReject_(producerId, messageId, priority, rejectReason, jobsUsed, highCount, normalCount, lowCount);
        }
        return false;
    }

    Job& job = jobs_[(uint8_t)idx];
    job = Job{};
    job.used = true;
    job.producerId = producerId;
    job.messageId = messageId;
    job.priority = priority;
    job.flags = flags;
    job.retryCount = 0;
    job.notBeforeMs = 0;

    ok = queueSlot_((uint8_t)idx, job.priority, false);
    if (!ok) {
        rejectReason = "queue_full";
        snapshotQueueStatsNoLock_(jobsUsed, highCount, normalCount, lowCount);
        shouldLogReject = !silentRejectLog;
        job = Job{};
    }

    portEXIT_CRITICAL(&jobsMux_);
    if (shouldLogReject) {
        logEnqueueReject_(producerId, messageId, priority, rejectReason, jobsUsed, highCount, normalCount, lowCount);
    }
    return ok;
}

bool MQTTModule::enqueue(uint8_t producerId, uint16_t messageId, MqttPublishPriority priority, uint8_t flags)
{
    if (producerId == 0U) return false;
    if (state_ != MQTTState::Connected) return false;
    return enqueueJob_(producerId, messageId, (uint8_t)priority, flags);
}

bool MQTTModule::dequeueNextJob_(uint32_t nowMs, uint8_t& slotIdx)
{
    static const uint8_t order[3] = {
        (uint8_t)MqttPublishPriority::High,
        (uint8_t)MqttPublishPriority::Normal,
        (uint8_t)MqttPublishPriority::Low
    };

    portENTER_CRITICAL(&jobsMux_);
    const uint16_t maxScan = highQ_.count + normalQ_.count + lowQ_.count + 4U;

    for (uint16_t scan = 0; scan < maxScan; ++scan) {
        for (uint8_t i = 0; i < 3; ++i) {
            JobQueueItem item{};
            if (!queuePop_(order[i], item)) continue;
            if (item.slot >= MaxJobs) continue;

            Job& job = jobs_[item.slot];
            if (!job.used || !job.queued || job.queueToken != item.token || job.queuedPrio != order[i]) {
                continue;
            }

            job.queued = false;
            if (job.processing) continue;

            if ((int32_t)(nowMs - job.notBeforeMs) < 0) {
                (void)queueSlot_(item.slot, job.priority, false);
                continue;
            }

            job.processing = true;
            slotIdx = item.slot;
            portEXIT_CRITICAL(&jobsMux_);
            return true;
        }
    }

    portEXIT_CRITICAL(&jobsMux_);
    return false;
}

bool MQTTModule::tryPublishNow_(const char* topic, const char* payload, uint8_t qos, bool retain)
{
    if (!topic || !payload) return false;
    if (state_ != MQTTState::Connected) return false;
    if (!client_) return false;

    const uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (freeHeap < Limits::NetworkPublish::MinFreeHeapBytes ||
        largest < Limits::NetworkPublish::MinLargestBlockBytes) {
        return false;
    }

    if (qos > 0U) {
        static constexpr int kMaxOutboxBytes = 12 * 1024;
        const int outboxBytes = esp_mqtt_client_get_outbox_size(client_);
        if (outboxBytes >= kMaxOutboxBytes) return false;
    }

    const int packetId = esp_mqtt_client_publish(client_, topic, payload, 0, qos, retain ? 1 : 0);
    return packetId >= 0;
}

void MQTTModule::processJobs_(uint32_t nowMs)
{
    for (uint8_t budget = 0; budget < ProcessBudgetPerTick; ++budget) {
        if (!scratch_) return;

        uint8_t slotIdx = 0;
        if (!dequeueNextJob_(nowMs, slotIdx)) break;

        uint8_t producerId = 0;
        uint16_t messageId = 0;
        {
            portENTER_CRITICAL(&jobsMux_);
            Job& job = jobs_[slotIdx];
            if (!job.used || !job.processing) {
                portEXIT_CRITICAL(&jobsMux_);
                continue;
            }
            producerId = job.producerId;
            messageId = job.messageId;
            portEXIT_CRITICAL(&jobsMux_);
        }

        const MqttPublishProducer* producer = findProducer_(producerId);
        MqttBuildResult buildResult = MqttBuildResult::PermanentError;
        bool published = false;

        memset(scratch_->topic, 0, sizeof(scratch_->topic));
        memset(scratch_->payload, 0, sizeof(scratch_->payload));

        MqttBuildContext ctx{};
        ctx.topic = scratch_->topic;
        ctx.topicCapacity = (uint16_t)sizeof(scratch_->topic);
        ctx.payload = scratch_->payload;
        ctx.payloadCapacity = (uint16_t)sizeof(scratch_->payload);

        if (producer && producer->buildMessage) {
            buildResult = producer->buildMessage(producer->ctx, messageId, ctx);
            if (buildResult == MqttBuildResult::Ready) {
                if (ctx.topicLen == 0U && ctx.topic[0] != '\0') {
                    ctx.topicLen = (uint16_t)strnlen(ctx.topic, ctx.topicCapacity);
                }
                if (ctx.payloadLen == 0U && ctx.payload[0] != '\0') {
                    ctx.payloadLen = (uint16_t)strnlen(ctx.payload, ctx.payloadCapacity);
                }

                if (ctx.topicLen == 0U || (ctx.payloadLen == 0U && !ctx.allowEmptyPayload)) {
                    buildResult = MqttBuildResult::PermanentError;
                } else {
                    BufferUsageTracker::note(TrackedBufferId::MqttPayloadBuf,
                                             ctx.payloadLen,
                                             sizeof(scratch_->payload),
                                             ctx.topic,
                                             nullptr);
                    published = tryPublishNow_(ctx.topic, ctx.payload, ctx.qos, ctx.retain);
                }
            }
        }

        bool callbackPublished = false;
        bool callbackDeferred = false;
        bool callbackDropped = false;

        portENTER_CRITICAL(&jobsMux_);
        Job& job = jobs_[slotIdx];
        if (!job.used) {
            portEXIT_CRITICAL(&jobsMux_);
            continue;
        }

        if (published) {
            job.processing = false;
            job.retryCount = 0;
            job.notBeforeMs = 0;
            if (job.requeueAfterProcess) {
                job.requeueAfterProcess = false;
                (void)queueSlot_(slotIdx, job.priority, false);
            } else {
                job = Job{};
            }
            callbackPublished = true;
        } else if (buildResult == MqttBuildResult::RetryLater ||
                   (buildResult == MqttBuildResult::Ready && !published)) {
            uint32_t backoff = RetryMinMs;
            if (job.retryCount > 0U) {
                backoff = (uint32_t)RetryMinMs << job.retryCount;
                if (backoff > RetryMaxMs) backoff = RetryMaxMs;
            }
            if (backoff > RetryMaxMs) backoff = RetryMaxMs;

            if (job.retryCount < 15U) ++job.retryCount;
            job.notBeforeMs = nowMs + backoff;
            job.processing = false;
            job.requeueAfterProcess = false;
            (void)queueSlot_(slotIdx, job.priority, false);
            callbackDeferred = true;
        } else {
            job.processing = false;
            if (job.requeueAfterProcess) {
                job.requeueAfterProcess = false;
                job.retryCount = 0;
                job.notBeforeMs = 0;
                (void)queueSlot_(slotIdx, job.priority, false);
                callbackDeferred = true;
            } else {
                job = Job{};
                callbackDropped = true;
            }
        }
        portEXIT_CRITICAL(&jobsMux_);

        if (producer) {
            if (callbackPublished && producer->onMessagePublished) {
                producer->onMessagePublished(producer->ctx, messageId);
            } else if (callbackDeferred && producer->onMessageDeferred) {
                producer->onMessageDeferred(producer->ctx, messageId);
            } else if (callbackDropped && producer->onMessageDropped) {
                producer->onMessageDropped(producer->ctx, messageId);
            }
        }
    }
}

void MQTTModule::updateAndReportQueueOccupancy_(uint32_t nowMs)
{
    uint16_t jobsUsed = 0U;
    uint16_t highCount = 0U;
    uint16_t normalCount = 0U;
    uint16_t lowCount = 0U;

    portENTER_CRITICAL(&jobsMux_);
    snapshotQueueStatsNoLock_(jobsUsed, highCount, normalCount, lowCount);
    portEXIT_CRITICAL(&jobsMux_);

    if (jobsUsed > occMaxJobs_) occMaxJobs_ = jobsUsed;
    if (highCount > occMaxHigh_) occMaxHigh_ = highCount;
    if (normalCount > occMaxNormal_) occMaxNormal_ = normalCount;
    if (lowCount > occMaxLow_) occMaxLow_ = lowCount;
    BufferUsageTracker::note(TrackedBufferId::MqttJobsAndQueues,
                             (size_t)jobsUsed * sizeof(Job) +
                                 (size_t)(highCount + normalCount + lowCount) * sizeof(JobQueueItem),
                             sizeof(jobs_) + sizeof(highQ_) + sizeof(normalQ_) + sizeof(lowQ_),
                             "occ",
                             nullptr);

    if (occLastReportMs_ == 0U) {
        occLastReportMs_ = nowMs;
        return;
    }
    if ((uint32_t)(nowMs - occLastReportMs_) < 5000U) return;

    LOGD("queue occ max/boot jobs=%u/%u qh=%u/%u qn=%u/%u ql=%u/%u",
         (unsigned)occMaxJobs_,
         (unsigned)MaxJobs,
         (unsigned)occMaxHigh_,
         (unsigned)HighQueueCap,
         (unsigned)occMaxNormal_,
         (unsigned)NormalQueueCap,
         (unsigned)occMaxLow_,
         (unsigned)LowQueueCap);

    occLastReportMs_ = nowMs;
}

void MQTTModule::clearAllJobs_(const char* reason)
{
    uint16_t jobsUsed = 0U;
    uint16_t highCount = 0U;
    uint16_t normalCount = 0U;
    uint16_t lowCount = 0U;

    portENTER_CRITICAL(&jobsMux_);
    snapshotQueueStatsNoLock_(jobsUsed, highCount, normalCount, lowCount);
    memset(jobs_, 0, sizeof(jobs_));
    highQ_ = JobRing<HighQueueCap>{};
    normalQ_ = JobRing<NormalQueueCap>{};
    lowQ_ = JobRing<LowQueueCap>{};
    portEXIT_CRITICAL(&jobsMux_);

    if (jobsUsed == 0U && highCount == 0U && normalCount == 0U && lowCount == 0U) return;

    LOGW("queue reset reason=%s jobs=%u qh=%u qn=%u ql=%u",
         reason ? reason : "unknown",
         (unsigned)jobsUsed,
         (unsigned)highCount,
         (unsigned)normalCount,
         (unsigned)lowCount);
}

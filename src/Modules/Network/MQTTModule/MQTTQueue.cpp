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
        const Job& job = txStorage_->jobs[i];
        if (job.state == JobState::Free) continue;
        if (job.producerId == producerId && job.messageId == messageId) return (int16_t)i;
    }
    return -1;
}

int16_t MQTTModule::allocJobSlot_()
{
    for (uint8_t i = 0; i < MaxJobs; ++i) {
        if (txStorage_->jobs[i].state == JobState::Free) return (int16_t)i;
    }
    return -1;
}

bool MQTTModule::queuePush_(uint8_t prio, const JobQueueItem& item)
{
    if (prio == (uint8_t)MqttPublishPriority::High) {
        if (txStorage_->highQ.count >= HighQueueCap) return false;
        txStorage_->highQ.items[txStorage_->highQ.tail] = item;
        txStorage_->highQ.tail = (uint16_t)((txStorage_->highQ.tail + 1U) % HighQueueCap);
        ++txStorage_->highQ.count;
        return true;
    }
    if (prio == (uint8_t)MqttPublishPriority::Normal) {
        if (txStorage_->normalQ.count >= NormalQueueCap) return false;
        txStorage_->normalQ.items[txStorage_->normalQ.tail] = item;
        txStorage_->normalQ.tail = (uint16_t)((txStorage_->normalQ.tail + 1U) % NormalQueueCap);
        ++txStorage_->normalQ.count;
        return true;
    }
    if (txStorage_->lowQ.count >= LowQueueCap) return false;
    txStorage_->lowQ.items[txStorage_->lowQ.tail] = item;
    txStorage_->lowQ.tail = (uint16_t)((txStorage_->lowQ.tail + 1U) % LowQueueCap);
    ++txStorage_->lowQ.count;
    return true;
}

bool MQTTModule::queuePop_(uint8_t prio, JobQueueItem& out)
{
    if (prio == (uint8_t)MqttPublishPriority::High) {
        if (txStorage_->highQ.count == 0U) return false;
        out = txStorage_->highQ.items[txStorage_->highQ.head];
        txStorage_->highQ.head = (uint16_t)((txStorage_->highQ.head + 1U) % HighQueueCap);
        --txStorage_->highQ.count;
        return true;
    }
    if (prio == (uint8_t)MqttPublishPriority::Normal) {
        if (txStorage_->normalQ.count == 0U) return false;
        out = txStorage_->normalQ.items[txStorage_->normalQ.head];
        txStorage_->normalQ.head = (uint16_t)((txStorage_->normalQ.head + 1U) % NormalQueueCap);
        --txStorage_->normalQ.count;
        return true;
    }
    if (txStorage_->lowQ.count == 0U) return false;
    out = txStorage_->lowQ.items[txStorage_->lowQ.head];
    txStorage_->lowQ.head = (uint16_t)((txStorage_->lowQ.head + 1U) % LowQueueCap);
    --txStorage_->lowQ.count;
    return true;
}

bool MQTTModule::queueSlot_(uint8_t slotIdx, uint8_t prio)
{
    if (slotIdx >= MaxJobs) return false;
    Job& job = txStorage_->jobs[slotIdx];
    if (job.state == JobState::Free || job.state == JobState::Processing) return false;
    if (job.state == JobState::Queued && job.queuedPrio == prio) return true;

    // Commit only after insertion succeeds: a failed promotion must leave the
    // previous entry valid. All callers hold jobsMux_ throughout this operation.
    JobQueueItem item{};
    item.slot = slotIdx;
    item.token = (uint16_t)(job.queueToken + 1U);
    if (!queuePush_(prio, item)) return false;

    job.queueToken = item.token;
    job.queuedPrio = prio;
    job.state = JobState::Queued;
    return true;
}

void MQTTModule::deferJob_(uint8_t slotIdx)
{
    Job& job = txStorage_->jobs[slotIdx];
    job.state = JobState::WaitingForQueue;
    // The consumer retries this durable state, respecting priority and backoff.
}

void MQTTModule::releaseJob_(uint8_t slotIdx)
{
    Job& job = txStorage_->jobs[slotIdx];
    // Obsolete references can outlive a slot's occupant. Preserve its generation
    // across reuse so they cannot become valid for the next message in this slot.
    if (job.state != JobState::Free) {
        ++txStorage_->counters.released;
        txStorage_->counters.residenceMs += (uint32_t)(millis() - job.admittedMs);
    }
    const uint16_t token = job.queueToken;
    job = Job{};
    job.queueToken = token;
}

void MQTTModule::retryPendingJobsNoLock_(uint32_t nowMs)
{
    for (int prio = (int)MqttPublishPriority::High; prio >= (int)MqttPublishPriority::Low; --prio) {
        const uint8_t start = queueRetryCursor_[prio];
        for (uint16_t n = 0; n < MaxJobs; ++n) {
            const uint8_t idx = (uint8_t)((start + n) % MaxJobs);
            Job& job = txStorage_->jobs[idx];
            const bool waiting = job.state == JobState::WaitingForQueue;
            const bool promotion = job.state == JobState::Queued && job.priority > job.queuedPrio;
            if ((!waiting && !promotion) || job.priority != prio) continue;
            if ((int32_t)(nowMs - job.notBeforeMs) < 0) continue;
            if (!queueSlot_(idx, job.priority)) break; // Target ring is full.
            queueRetryCursor_[prio] = (uint8_t)((idx + 1U) % MaxJobs);
        }
    }
}

void MQTTModule::snapshotQueueStatsNoLock_(QueueSnapshot& snapshot, uint32_t nowMs) const
{
    snapshot = QueueSnapshot{};
    snapshot.atMs = nowMs;
    snapshot.counters = txStorage_->counters;
    uint16_t references[MaxJobs]{};
    auto inspect = [&](const auto& ring, uint8_t priority) {
        const size_t capacity = sizeof(ring.items) / sizeof(ring.items[0]);
        snapshot.physical[priority] = ring.count;
        for (uint16_t n = 0; n < ring.count; ++n) {
            const JobQueueItem& item = ring.items[(ring.head + n) % capacity];
            if (item.slot < MaxJobs) {
                const Job& job = txStorage_->jobs[item.slot];
                if (job.state == JobState::Queued && job.queueToken == item.token && job.queuedPrio == priority) {
                    ++snapshot.live[priority];
                    ++references[item.slot];
                    continue;
                }
            }
            ++snapshot.stale[priority];
        }
    };
    inspect(txStorage_->lowQ, 0);
    inspect(txStorage_->normalQ, 1);
    inspect(txStorage_->highQ, 2);
    for (uint16_t i = 0; i < MaxJobs; ++i) {
        const Job& job = txStorage_->jobs[i];
        if (references[i] != (job.state == JobState::Queued ? 1U : 0U)) snapshot.invariant = false;
        if (job.state == JobState::Free) {
            ++snapshot.free;
            continue;
        }
        ++snapshot.used;
        const uint32_t age = nowMs - job.admittedMs;
        if (age > snapshot.oldestMs) snapshot.oldestMs = age;
        switch (job.state) {
            case JobState::Queued: ++snapshot.states.queued; break;
            case JobState::Processing: ++snapshot.states.processing; break;
            case JobState::WaitingForQueue: ++snapshot.states.waiting; break;
            case JobState::Free: break;
        }
    }
    snapshot.invariant = snapshot.invariant &&
        snapshot.free + snapshot.used == MaxJobs &&
        snapshot.used == snapshot.states.queued + snapshot.states.processing + snapshot.states.waiting &&
        snapshot.states.queued == snapshot.live[0] + snapshot.live[1] + snapshot.live[2] &&
        (uint32_t)(snapshot.counters.admitted - snapshot.counters.released) == snapshot.used;
}

void MQTTModule::logQueueSnapshot_(const QueueSnapshot& snapshot) const
{
    // LogHub caps each message at 160 bytes. The timestamp joins these lines
    // to one immutable snapshot even if another task logs between them.
    LOGI("queue at=%lu used=%u free=%u queued=%u processing=%u waiting=%u invariant=%u",
         (unsigned long)snapshot.atMs, (unsigned)snapshot.used, (unsigned)snapshot.free,
         (unsigned)snapshot.states.queued, (unsigned)snapshot.states.processing,
         (unsigned)snapshot.states.waiting, (unsigned)snapshot.invariant);
    LOGI("queue at=%lu h/n/l physical=%u/%u/%u live=%u/%u/%u stale=%u/%u/%u oldest_ms=%lu",
         (unsigned long)snapshot.atMs,
         (unsigned)snapshot.physical[2], (unsigned)snapshot.physical[1], (unsigned)snapshot.physical[0],
         (unsigned)snapshot.live[2], (unsigned)snapshot.live[1], (unsigned)snapshot.live[0],
         (unsigned)snapshot.stale[2], (unsigned)snapshot.stale[1], (unsigned)snapshot.stale[0],
         (unsigned long)snapshot.oldestMs);
}

void MQTTModule::logEnqueueIssue_(uint8_t producerId, uint16_t messageId, uint8_t priority,
                                 const char* reason, const QueueSnapshot& snapshot, bool accepted)
{
    const auto& source = MqttDiagnostics::Sources[MqttDiagnostics::sourceIndex(producerId, messageId)];
    LOGW("enqueue %s reason=%s producer=%u msg=%u type=%u source=%s prio=%u at=%lu",
         accepted ? "deferred" : "reject", reason, (unsigned)producerId, (unsigned)messageId,
         (unsigned)source.messageTypeId, source.name, (unsigned)priority, (unsigned long)snapshot.atMs);
    logQueueSnapshot_(snapshot);
}

bool MQTTModule::enqueueJob_(uint8_t producerId, uint16_t messageId, uint8_t priority, uint8_t flags)
{
    bool accepted = false;
    const char* issue = nullptr;
    const bool silent = (flags & (uint8_t)MqttEnqueueFlags::SilentRejectLog) != 0U;
    QueueSnapshot snapshot{};
    bool report = false;
    portENTER_CRITICAL(&jobsMux_);
    const uint32_t nowMs = millis();

    int16_t idx = findJobSlot_(producerId, messageId);
    if (idx >= 0) {
        ++txStorage_->counters.coalesced;
        Job& job = txStorage_->jobs[(uint8_t)idx];
        job.flags |= flags;
        if (priority > job.priority) job.priority = priority;
        // The transport already owns this publication, even if promotion or
        // readmission must wait. Do not ask producers to retry an accepted job.
        accepted = true;
        if (job.state == JobState::Processing) {
            job.requeueAfterProcess = true;
        } else if (job.state == JobState::Queued) {
            if (job.priority > job.queuedPrio && !queueSlot_((uint8_t)idx, job.priority)) {
                issue = "promotion_full";
            }
        }
        // WaitingForQueue is retried by the consumer without resetting backoff.
    } else {
        idx = allocJobSlot_();
        if (idx < 0) {
            issue = "slot_full";
        } else {
            Job& job = txStorage_->jobs[(uint8_t)idx];
            job.admittedMs = nowMs;
            job.state = JobState::WaitingForQueue;
            job.producerId = producerId;
            job.messageId = messageId;
            job.priority = priority;
            job.flags = flags;
            accepted = queueSlot_((uint8_t)idx, job.priority);
            if (!accepted) {
                // Admission failed: this slot never became an occupied job.
                job.state = JobState::Free;
                releaseJob_((uint8_t)idx);
                issue = "queue_full";
            } else {
                ++txStorage_->counters.admitted;
            }
        }
    }
    if (!accepted) ++txStorage_->counters.rejected[MqttDiagnostics::sourceIndex(producerId, messageId)][priority];
    // Rate limiting and the snapshot use the same lock as every job transition.
    if (issue && !silent && (lastEnqueueIssueLogMs_ == 0U || (uint32_t)(nowMs - lastEnqueueIssueLogMs_) >= 1000U)) {
        lastEnqueueIssueLogMs_ = nowMs;
        snapshotQueueStatsNoLock_(snapshot, nowMs);
        report = true;
    }
    portEXIT_CRITICAL(&jobsMux_);
    if (report) logEnqueueIssue_(producerId, messageId, priority, issue, snapshot, accepted);
    return accepted;
}

bool MQTTModule::enqueue(uint8_t producerId, uint16_t messageId, MqttPublishPriority priority, uint8_t flags)
{
    if (!txStorage_ || producerId == 0U || (uint8_t)priority > (uint8_t)MqttPublishPriority::High) return false;
    if (state_ != MQTTState::Connected) return false;
    return enqueueJob_(producerId, messageId, (uint8_t)priority, flags);
}

bool MQTTModule::dequeueNextJob_(uint32_t nowMs, uint8_t& slotIdx)
{
    portENTER_CRITICAL(&jobsMux_);
    retryPendingJobsNoLock_(nowMs);
    for (int prio = (int)MqttPublishPriority::High; prio >= (int)MqttPublishPriority::Low; --prio) {
        // Each physical entry is examined at most once per priority per pass.
        const uint16_t count = prio == (int)MqttPublishPriority::High ? txStorage_->highQ.count
            : prio == (int)MqttPublishPriority::Normal ? txStorage_->normalQ.count : txStorage_->lowQ.count;
        for (uint16_t scan = 0; scan < count; ++scan) {
            JobQueueItem item{};
            if (!queuePop_((uint8_t)prio, item)) break;
            if (item.slot >= MaxJobs) continue;
            Job& job = txStorage_->jobs[item.slot];
            if (job.state != JobState::Queued || job.queueToken != item.token || job.queuedPrio != prio) continue;

            if ((int32_t)(nowMs - job.notBeforeMs) < 0) {
                // Keep delayed work outside the rings until due. This also
                // releases capacity for ready jobs without bypassing backoff.
                job.state = JobState::WaitingForQueue;
                continue;
            }
            job.state = JobState::Processing;
            ++txStorage_->counters.dequeued;
            slotIdx = item.slot;
            // Offer the newly freed capacity to retained work before producers
            // can fill it again, preserving round-robin admission progress.
            retryPendingJobsNoLock_(nowMs);
            portEXIT_CRITICAL(&jobsMux_);
            return true;
        }
    }
    retryPendingJobsNoLock_(nowMs);
    portEXIT_CRITICAL(&jobsMux_);
    return false;
}

bool MQTTModule::tryPublishNow_(const char* topic, const char* payload, uint8_t qos, bool retain)
{
    if (!topic || !payload) return false;
    if (state_ != MQTTState::Connected) return false;
    if (!client_) return false;

    const uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t freeInternal = heap_caps_get_free_size(internalCaps);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(internalCaps);
    if (freeInternal < Limits::NetworkPublish::MinInternalFreeBytes ||
        largestInternal < Limits::NetworkPublish::MinInternalLargestBlockBytes) {
        const uint32_t nowMs = millis();
        if (lastMemoryGuardLogMs_ == 0U ||
            (uint32_t)(nowMs - lastMemoryGuardLogMs_) >= 5000U) {
            lastMemoryGuardLogMs_ = nowMs;
            LOGW("publish deferred: internal memory reserve free=%lu largest=%lu required_free=%lu required_largest=%lu",
                 (unsigned long)freeInternal,
                 (unsigned long)largestInternal,
                 (unsigned long)Limits::NetworkPublish::MinInternalFreeBytes,
                 (unsigned long)Limits::NetworkPublish::MinInternalLargestBlockBytes);
        }
        return false;
    }

    if (qos > 0U) {
        const int outboxBytes = esp_mqtt_client_get_outbox_size(client_);
        if (outboxBytes < 0 ||
            (uint64_t)outboxBytes >= Limits::Mqtt::Client::OutboxLimitBytes) {
            return false;
        }
    }

    const int packetId = esp_mqtt_client_publish(client_, topic, payload, 0, qos, retain ? 1 : 0);
    return packetId >= 0;
}

void MQTTModule::processJobs_(uint32_t nowMs)
{
    if (state_ != MQTTState::Connected) return;
    portENTER_CRITICAL(&jobsMux_);
    retryPendingJobsNoLock_(nowMs);
    portEXIT_CRITICAL(&jobsMux_);
    if (lastPublishDispatchMs_ != 0U &&
        (uint32_t)(nowMs - lastPublishDispatchMs_) <
            Limits::Mqtt::Timing::PublishDispatchIntervalMs) {
        return;
    }

    for (uint8_t budget = 0; budget < ProcessBudgetPerTick; ++budget) {
        if (!scratch_) return;

        uint8_t slotIdx = 0;
        if (!dequeueNextJob_(nowMs, slotIdx)) break;

        uint8_t producerId = 0;
        uint16_t messageId = 0;
        {
            portENTER_CRITICAL(&jobsMux_);
            Job& job = txStorage_->jobs[slotIdx];
            if (job.state != JobState::Processing) {
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
                    lastPublishDispatchMs_ = nowMs;
                    published = tryPublishNow_(ctx.topic, ctx.payload, ctx.qos, ctx.retain);
                }
            }
        }

        bool callbackPublished = false;
        bool callbackDeferred = false;
        bool callbackDropped = false;

        portENTER_CRITICAL(&jobsMux_);
        Job& job = txStorage_->jobs[slotIdx];
        if (job.state != JobState::Processing) {
            portEXIT_CRITICAL(&jobsMux_);
            continue;
        }

        if (published) {
            job.retryCount = 0;
            job.notBeforeMs = 0;
            if (job.requeueAfterProcess) {
                job.requeueAfterProcess = false;
                deferJob_(slotIdx);
            } else {
                releaseJob_(slotIdx);
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
            job.requeueAfterProcess = false;
            deferJob_(slotIdx);
            callbackDeferred = true;
        } else {
            if (job.requeueAfterProcess) {
                job.requeueAfterProcess = false;
                job.retryCount = 0;
                job.notBeforeMs = 0;
                deferJob_(slotIdx);
                callbackDeferred = true;
            } else {
                releaseJob_(slotIdx);
                callbackDropped = true;
            }
        }
        retryPendingJobsNoLock_(nowMs);
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
    QueueSnapshot snapshot{};
    portENTER_CRITICAL(&jobsMux_);
    nowMs = millis();
    snapshotQueueStatsNoLock_(snapshot, nowMs);
    portEXIT_CRITICAL(&jobsMux_);
    const uint16_t jobsUsed = snapshot.used;
    const uint16_t highCount = snapshot.physical[2];
    const uint16_t normalCount = snapshot.physical[1];
    const uint16_t lowCount = snapshot.physical[0];

    if (jobsUsed > occMaxJobs_) occMaxJobs_ = jobsUsed;
    if (highCount > occMaxHigh_) occMaxHigh_ = highCount;
    if (normalCount > occMaxNormal_) occMaxNormal_ = normalCount;
    if (lowCount > occMaxLow_) occMaxLow_ = lowCount;
    BufferUsageTracker::note(TrackedBufferId::MqttJobsAndQueues,
                             (size_t)jobsUsed * sizeof(Job) +
                                 (size_t)(highCount + normalCount + lowCount) * sizeof(JobQueueItem),
                             sizeof(txStorage_->jobs) + sizeof(txStorage_->highQ) + sizeof(txStorage_->normalQ) + sizeof(txStorage_->lowQ),
                             "occ",
                             nullptr);

    if (occLastReportMs_ != 0U && (uint32_t)(nowMs - occLastReportMs_) < 5000U) return;

    LOGD("queue occ max/boot jobs=%u/%u qh=%u/%u qn=%u/%u ql=%u/%u",
         (unsigned)occMaxJobs_,
         (unsigned)MaxJobs,
         (unsigned)occMaxHigh_,
         (unsigned)HighQueueCap,
         (unsigned)occMaxNormal_,
         (unsigned)NormalQueueCap,
         (unsigned)occMaxLow_,
         (unsigned)LowQueueCap);

    const uint32_t interval = nowMs - occLastReportMs_;
    const uint32_t elapsed = interval == 0U ? 1U : interval;
    const auto& counters = snapshot.counters;
    logQueueSnapshot_(snapshot);
    LOGI("queue at=%lu enqueue_mHz=%lu dequeue_mHz=%lu released=%lu mean_slot_ms=%lu coalesced=%lu",
         (unsigned long)snapshot.atMs,
         (unsigned long)((uint64_t)(counters.admitted - lastReportedCounters_.admitted) * 1000000U / elapsed),
         (unsigned long)((uint64_t)(counters.dequeued - lastReportedCounters_.dequeued) * 1000000U / elapsed),
         (unsigned long)counters.released,
         (unsigned long)(counters.released ? counters.residenceMs / counters.released : 0U),
         (unsigned long)counters.coalesced);
    for (size_t i = 0; i < MqttDiagnostics::SourceCount; ++i) {
        for (uint8_t priority = 0; priority < 3; ++priority) {
            const uint32_t total = counters.rejected[i][priority];
            const uint32_t delta = total - lastReportedCounters_.rejected[i][priority];
            if (delta == 0U) continue;
            const auto& source = MqttDiagnostics::Sources[i];
            LOGW("queue rejects at=%lu producer=%u type=%u source=%s prio=%u delta=%lu total=%lu",
                 (unsigned long)snapshot.atMs, (unsigned)source.producerId, (unsigned)source.messageTypeId,
                 source.name, (unsigned)priority, (unsigned long)delta, (unsigned long)total);
        }
    }
    lastReportedCounters_ = counters;
    occLastReportMs_ = nowMs;
}

#include "EventBus.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>

#include "Core/BufferUsageTracker.h"
#include "Core/Log.h"
#include "Core/SaturatingCounter.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::CoreEventBus)

namespace {
#if FLOW_PERFORMANCE_DIAGNOSTICS && EVENTBUS_PROFILE
uint32_t lastDispatchWarnMs = 0;
uint32_t lastHandlerWarnMs = 0;

bool canWarnNow(uint32_t& lastWarnMs)
{
    const uint32_t now = millis();
    if (now - lastWarnMs < EVENTBUS_WARN_MIN_INTERVAL_MS) return false;
    lastWarnMs = now;
    return true;
}
#endif

bool isControlEvent(EventId id)
{
    switch (id) {
        case EventId::ConfigChanged:
        case EventId::RelayChanged:
        case EventId::PoolModeChanged:
        case EventId::AlarmRaised:
        case EventId::AlarmCleared:
        case EventId::AlarmReset:
        case EventId::AlarmSilenceChanged:
        case EventId::AlarmConditionChanged:
        case EventId::SchedulerEventTriggered:
            return true;
        default:
            return false;
    }
}
}  // namespace

EventBus::EventBus()
{
    notePeakUsage_(0, 0, false);
}

void EventBus::notePeakUsage_(uint8_t queued, uint16_t subscribers, bool fromIsr)
{
    const size_t used = queued * sizeof(QueuedEvent) + subscribers * sizeof(Subscriber);
    const size_t capacity = sizeof(_queue) + sizeof(_subs);
    if (fromIsr) {
        BufferUsageTracker::noteFromISR(TrackedBufferId::EventBusRuntime, used, capacity, "eventbus", nullptr);
    } else {
        BufferUsageTracker::note(TrackedBufferId::EventBusRuntime, used, capacity, "eventbus", nullptr);
    }
}

bool EventBus::subscribe(EventId id, EventCallback cb, void* user)
{
    if (!cb) {
        recordSubscribeReject_(id, cb, user, SubRejectReason::NullCb);
        return false;
    }

    // Publish the subscriber before incrementing the count seen by dispatch.
    portENTER_CRITICAL(&_mux);
    const bool hasRoom = _count < MAX_SUBSCRIBERS;
    if (hasRoom) _subs[_count++] = {id, cb, user};
    const uint16_t count = _count;
    const uint8_t queued = _stats.queued;
    portEXIT_CRITICAL(&_mux);
    if (!hasRoom) {
        recordSubscribeReject_(id, cb, user, SubRejectReason::Capacity);
        return false;
    }
    notePeakUsage_(queued, count, false);
    return true;
}

void EventBus::recordSubscribeReject_(EventId id, EventCallback cb, void* user, SubRejectReason reason)
{
    const uint32_t now = millis();
    portENTER_CRITICAL(&_mux);
    if (_stats.subscriptionDrops < UINT32_MAX) {
        ++_stats.subscriptionDrops;
        _subRejectRing[_subRejectWriteIdx] = {_stats.subscriptionDrops, now,
                                              static_cast<uint16_t>(id),
                                              reinterpret_cast<uintptr_t>(cb),
                                              reinterpret_cast<uintptr_t>(user),
                                              static_cast<uint8_t>(reason)};
        _subRejectWriteIdx = (_subRejectWriteIdx + 1U) % SUB_REJECT_RING_CAP;
    }
    portEXIT_CRITICAL(&_mux);
}

const char* EventBus::subRejectReasonStr_(uint8_t reason)
{
    switch (static_cast<SubRejectReason>(reason)) {
        case SubRejectReason::NullCb: return "null_cb";
        case SubRejectReason::Capacity: return "capacity";
        default: return "unknown";
    }
}

bool EventBus::post(EventId id, const void* payload, size_t len, ModuleId producer)
{
    return post_(id, payload, len, producer, PostMode::DiscardOnFull);
}

bool EventBus::tryPost(EventId id, const void* payload, size_t len, ModuleId producer)
{
    return post_(id, payload, len, producer, PostMode::RetryOnFull);
}

bool EventBus::postFromISR(EventId id, const void* payload, size_t len, ModuleId producer)
{
    return post_(id, payload, len, producer, PostMode::FromIsr);
}

bool EventBus::post_(EventId id, const void* payload, size_t len, ModuleId producer, PostMode mode)
{
    const bool fromIsr = mode == PostMode::FromIsr;
    const bool control = isControlEvent(id);
    if (fromIsr) {
        portENTER_CRITICAL_ISR(&_mux);
    } else {
        portENTER_CRITICAL(&_mux);
    }

    uint32_t* drop = nullptr;
    bool accepted = false;
    bool newPeak = false;
    if (len > MAX_PAYLOAD_SIZE) {
        drop = &_stats.oversizeDrops;
    } else if (len != 0U && !payload) {
        drop = &_stats.missingPayloadDrops;
    } else if (_stats.queued == QUEUE_LENGTH) {
        if (mode == PostMode::RetryOnFull) {
            incrementSaturated(_stats.fullRetries);
        } else {
            drop = &_stats.fullDrops;
        }
    } else {
        QueuedEvent& entry = _queue[_tail];
#if FLOW_PERFORMANCE_DIAGNOSTICS
        entry.admittedUs = esp_timer_get_time();
#endif
        entry.id = id;
        entry.producer = producer;
        entry.len = static_cast<uint8_t>(len);
        if (len != 0U) memcpy(entry.data, payload, len);
        _tail = (_tail + 1U) % QUEUE_LENGTH;
        ++_stats.queued;
        incrementSaturated(_stats.accepted);
        if (_stats.queued > _stats.peak) {
            _stats.peak = _stats.queued;
            newPeak = true;
        }
        accepted = true;
    }
    if (drop) {
        incrementSaturated(*drop);
        if (fromIsr) incrementSaturated(_stats.isrDrops);
        if (control) incrementSaturated(_stats.controlDrops);
        _stats.lastDroppedEvent = id;
        _stats.lastDroppedProducer = producer;
    }
    const uint8_t queued = _stats.queued;
    const uint16_t subscribers = _count;
    if (fromIsr) {
        portEXIT_CRITICAL_ISR(&_mux);
    } else {
        portEXIT_CRITICAL(&_mux);
    }

    if (newPeak) notePeakUsage_(queued, subscribers, fromIsr);
    return accepted;
}

EventBus::QueueSnapshot EventBus::queueSnapshot()
{
    portENTER_CRITICAL(&_mux);
    QueueSnapshot snapshot = _stats;
    if (snapshot.queued != 0U) {
        const QueuedEvent& head = _queue[_head];
        snapshot.oldestEvent = head.id;
        snapshot.oldestProducer = head.producer;
#if FLOW_PERFORMANCE_DIAGNOSTICS
        snapshot.oldestAgeUs = esp_timer_get_time() - head.admittedUs;
#endif
    }
    portEXIT_CRITICAL(&_mux);
    return snapshot;
}

bool EventBus::take_(QueuedEvent& event)
{
    portENTER_CRITICAL(&_mux);
    const bool available = _stats.queued != 0U;
    if (available) {
        event = _queue[_head];
        _head = (_head + 1U) % QUEUE_LENGTH;
        --_stats.queued;
        incrementSaturated(_stats.dequeued);
#if FLOW_PERFORMANCE_DIAGNOSTICS
        const uint64_t delay = esp_timer_get_time() - event.admittedUs;
        if (delay >= _stats.maxQueueDelayUs) {
            _stats.maxQueueDelayUs = delay;
            _stats.slowestEvent = event.id;
            _stats.slowestProducer = event.producer;
        }
#endif
    }
    portEXIT_CRITICAL(&_mux);
    return available;
}

void EventBus::reportSubscribeRejects_()
{
    for (uint8_t i = 0; i < SUB_REJECT_RING_CAP; ++i) {
        SubscribeRejectInfo next{};
        portENTER_CRITICAL(&_mux);
        for (const auto& reject : _subRejectRing) {
            if (reject.seq > _subRejectReportedSeq && (next.seq == 0U || reject.seq < next.seq)) {
                next = reject;
            }
        }
        portEXIT_CRITICAL(&_mux);
        if (!next.seq) break;
        _subRejectReportedSeq = next.seq;
        Log::warn(LOG_MODULE_ID,
                  "sub reject seq=%lu event=%u reason=%s cb=0x%lx user=0x%lx age_ms=%lu",
                  (unsigned long)next.seq, (unsigned)next.eventId, subRejectReasonStr_(next.reason),
                  (unsigned long)next.cbAddr, (unsigned long)next.userAddr,
                  (unsigned long)(millis() - next.tsMs));
    }
}

void EventBus::dispatch(uint16_t maxEvents)
{
    reportSubscribeRejects_();
#if FLOW_PERFORMANCE_DIAGNOSTICS && EVENTBUS_PROFILE
    const uint32_t startedUs = micros();
    const auto queuedBefore = queueSnapshot().queued;
#endif
    uint16_t dispatched = 0;
    for (; dispatched < maxEvents; ++dispatched) {
        QueuedEvent event;
        if (!take_(event)) break;
        dispatchOne(event);
    }
#if FLOW_PERFORMANCE_DIAGNOSTICS && EVENTBUS_PROFILE
    const uint32_t elapsedUs = micros() - startedUs;
    if (dispatched > 0U && elapsedUs > EVENTBUS_DISPATCH_WARN_US && canWarnNow(lastDispatchWarnMs)) {
        Log::warn(LOG_MODULE_ID, "dispatch slow: events=%u dt=%lu us queue=%u->%u/%u",
                  (unsigned)dispatched, (unsigned long)elapsedUs, (unsigned)queuedBefore,
                  (unsigned)queueSnapshot().queued, (unsigned)QUEUE_LENGTH);
    }
#endif
}

void EventBus::dispatchOne(const QueuedEvent& qe) {
    Event e;
    e.id = qe.id;
    e.payload = (qe.len > 0) ? qe.data : nullptr;
    e.len = qe.len;

    portENTER_CRITICAL(&_mux);
    const uint16_t count = _count;
    portEXIT_CRITICAL(&_mux);
    for (uint16_t i = 0; i < count; ++i) {
        if (_subs[i].id == qe.id && _subs[i].cb != nullptr) {

#if FLOW_PERFORMANCE_DIAGNOSTICS && EVENTBUS_PROFILE
            const uint32_t t0 = micros();
#endif

            _subs[i].cb(e, _subs[i].user);

#if FLOW_PERFORMANCE_DIAGNOSTICS && EVENTBUS_PROFILE
            const uint32_t dt = (uint32_t)(micros() - t0);
            if (dt > EVENTBUS_HANDLER_WARN_US && canWarnNow(lastHandlerWarnMs)) {
                const auto queuedAfter = queueSnapshot().queued;
                Log::warn(LOG_MODULE_ID,
                          "slow handler: event=%u producer=%u sub=%u/%u cb=%p user=%p dt=%lu us queue=%u/%u",
                          (unsigned)qe.id,
                          (unsigned)qe.producer,
                          (unsigned)i,
                          (unsigned)count,
                          (void*)_subs[i].cb,
                          _subs[i].user,
                          (unsigned long)dt,
                          (unsigned)queuedAfter,
                          (unsigned)QUEUE_LENGTH);
            }
#endif
        }
    }
}

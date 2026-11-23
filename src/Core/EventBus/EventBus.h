#pragma once
/**
 * @file EventBus.h
 * @brief Simple queued event bus with fixed-size payloads.
 */
#include <stdint.h>
#include <stddef.h>

#include "EventId.h"
#include "Core/PerformanceDiagnostics.h"
#include "Core/ModuleId.h"
#include "Core/SystemLimits.h"

#include "freertos/FreeRTOS.h"

#ifndef EVENTBUS_PROFILE
#define EVENTBUS_PROFILE FLOW_PERFORMANCE_DIAGNOSTICS
#endif

#ifndef EVENTBUS_HANDLER_WARN_US
#define EVENTBUS_HANDLER_WARN_US 7000  // 7ms
#endif

#ifndef EVENTBUS_DISPATCH_WARN_US
#define EVENTBUS_DISPATCH_WARN_US 20000 // 20ms for a batch
#endif

#ifndef EVENTBUS_WARN_MIN_INTERVAL_MS
#define EVENTBUS_WARN_MIN_INTERVAL_MS 2000
#endif

/** @brief Event delivered to subscribers during dispatch(). */
struct Event {
    EventId id;
    const void* payload;
    size_t len;
};

/** @brief Callback signature for event subscribers. */
using EventCallback = void(*)(const Event& e, void* user);

/**
 * @brief Thread-safe event queue with subscriber dispatch.
 */
class EventBus {
public:
    static constexpr uint16_t MAX_SUBSCRIBERS = Limits::EventSubscribersMax;
    static constexpr uint8_t SUB_REJECT_RING_CAP = 8;
    // Maximum payload size copied into internal queue.
    static constexpr uint8_t MAX_PAYLOAD_SIZE = 48;

    // Maximum number of queued events.
    static constexpr uint8_t QUEUE_LENGTH = Limits::EventQueueLen;

    enum class QueueId : uint8_t { Dispatch = 1 };

    struct QueueSnapshot {
        QueueId id = QueueId::Dispatch;
        uint8_t capacity = QUEUE_LENGTH;
        uint8_t queued = 0;
        uint8_t peak = 0;
        uint32_t accepted = 0;
        uint32_t dequeued = 0;
        uint32_t fullDrops = 0;
        uint32_t fullRetries = 0;
        uint32_t oversizeDrops = 0;
        uint32_t missingPayloadDrops = 0;
        uint32_t isrDrops = 0;
        uint32_t controlDrops = 0;
        uint32_t subscriptionDrops = 0;
        EventId lastDroppedEvent = EventId::None;
        ModuleId lastDroppedProducer = ModuleId::Unknown;
        EventId oldestEvent = EventId::None;
        ModuleId oldestProducer = ModuleId::Unknown;
        uint64_t oldestAgeUs = 0;
        EventId slowestEvent = EventId::None;
        ModuleId slowestProducer = ModuleId::Unknown;
        uint64_t maxQueueDelayUs = 0;
    };

    EventBus();
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    /** @brief Append a subscriber. Its callback and context must outlive dispatch. */
    bool subscribe(EventId id, EventCallback cb, void* user);

    /**
     * @brief Post an event from any task; payload is copied into the queue.
     */
    bool post(EventId id,
              const void* payload = nullptr,
              size_t len = 0,
              ModuleId producer = ModuleId::Unknown);

    /** @brief Attempt delivery without counting a full queue as a loss.
     * The caller owns the pending event and must retry until accepted.
     * Invalid payloads remain errors. */
    bool tryPost(EventId id, const void* payload = nullptr, size_t len = 0,
                 ModuleId producer = ModuleId::Unknown);

    /** @brief Post an event from ISR context. */
    bool postFromISR(EventId id,
                     const void* payload = nullptr,
                     size_t len = 0,
                     ModuleId producer = ModuleId::Unknown);

    /** @brief Dispatch queued events and call subscribers from the single consumer task. */
    void dispatch(uint16_t maxEvents = 8);

    /** @brief Copy a consistent snapshot from task context. */
    QueueSnapshot queueSnapshot();

private:
    enum class SubRejectReason : uint8_t {
        None = 0,
        NullCb = 1,
        Capacity = 2,
    };

    struct Subscriber {
        EventId id;
        EventCallback cb;
        void* user;
    };

    struct SubscribeRejectInfo {
        uint32_t seq = 0;
        uint32_t tsMs = 0;
        uint16_t eventId = 0;
        uintptr_t cbAddr = 0;
        uintptr_t userAddr = 0;
        uint8_t reason = (uint8_t)SubRejectReason::None;
    };

    struct QueuedEvent {
#if FLOW_PERFORMANCE_DIAGNOSTICS
        uint64_t admittedUs;
#endif
        alignas(uint64_t) uint8_t data[MAX_PAYLOAD_SIZE];
        EventId id;
        ModuleId producer;
        uint8_t len;
    };

    Subscriber _subs[MAX_SUBSCRIBERS];
    uint16_t _count = 0;

    QueuedEvent _queue[QUEUE_LENGTH]{};
    uint8_t _head = 0;
    uint8_t _tail = 0;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    QueueSnapshot _stats{};
    uint32_t _subRejectReportedSeq = 0;
    SubscribeRejectInfo _subRejectRing[SUB_REJECT_RING_CAP]{};
    uint8_t _subRejectWriteIdx = 0;

    enum class PostMode : uint8_t { DiscardOnFull, RetryOnFull, FromIsr };
    bool post_(EventId id, const void* payload, size_t len, ModuleId producer, PostMode mode);
    bool take_(QueuedEvent& event);
    void reportSubscribeRejects_();
    void notePeakUsage_(uint8_t queued, uint16_t subscribers, bool fromIsr);
    void dispatchOne(const QueuedEvent& qe);
    void recordSubscribeReject_(EventId id, EventCallback cb, void* user, SubRejectReason reason);
    static const char* subRejectReasonStr_(uint8_t reason);
};

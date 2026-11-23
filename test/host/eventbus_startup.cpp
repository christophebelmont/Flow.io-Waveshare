#include "Core/EventBus/EventBus.h"
#include "Core/EventBus/EventPayloads.h"
#include "Core/DataStore/PendingDataKeys.h"
#include "Core/Log.h"
#include <Arduino.h>
#include <cassert>
#include <esp_timer.h>
#include <thread>

void testConcurrentProducers()
{
    EventBus bus;
    constexpr unsigned attempts = 2000;
    unsigned received = 0;
    bus.subscribe(EventId::ConfigChanged, [](const Event& event, void* context) {
        assert(event.len == sizeof(uint32_t));
        assert(*static_cast<const uint32_t*>(event.payload) < attempts);
        ++*static_cast<unsigned*>(context);
    }, &received);

    std::thread task([&] {
        for (uint32_t value = 0; value < attempts; ++value) {
            bus.post(EventId::ConfigChanged, &value, sizeof(value), ModuleId::ConfigStore);
        }
    });
    std::thread interrupt([&] {
        for (uint32_t value = 0; value < attempts; ++value) {
            bus.postFromISR(EventId::ConfigChanged, &value, sizeof(value), ModuleId::ConfigStore);
        }
    });
    for (unsigned pass = 0; pass < attempts; ++pass) {
        bus.dispatch(4);
        const auto snapshot = bus.queueSnapshot();
        assert(snapshot.accepted == snapshot.dequeued + snapshot.queued);
        assert(snapshot.queued <= snapshot.peak && snapshot.peak <= snapshot.capacity);
    }
    task.join();
    interrupt.join();
    bus.dispatch(EventBus::QUEUE_LENGTH);
    const auto snapshot = bus.queueSnapshot();
    assert(snapshot.accepted + snapshot.fullDrops == 2 * attempts);
    assert(snapshot.dequeued == received && snapshot.accepted == received);
    assert(snapshot.fullDrops == snapshot.controlDrops && snapshot.isrDrops <= snapshot.fullDrops);
}

int main() {
    EventBus bus;
    PendingDataKeys changes;
    unsigned received[DataKeys::ReservedMax + 1]{};
    unsigned transitions = 0, started = 0;
    bus.subscribe(EventId::DataChanged, [](const Event& e, void* ctx) {
        const auto key = static_cast<const DataChangedPayload*>(e.payload)->id;
        ++static_cast<unsigned*>(ctx)[key];
    }, received);
    bus.subscribe(EventId::SystemStarted, [](const Event&, void* ctx) {
        ++*static_cast<unsigned*>(ctx);
    }, &started);
    bus.subscribe(EventId::ConfigChanged, [](const Event& e, void* ctx) {
        auto& count = *static_cast<unsigned*>(ctx);
        assert(*static_cast<const unsigned*>(e.payload) == count++);
    }, &transitions);
    for (DataKey key = 0; key <= DataKeys::ReservedMax; ++key) {
        assert(changes.mark(key));
        assert(changes.mark(key));
    }
    // Ordinary events occupy the entire queue: retained events must retry.
    for (unsigned i = 0; i < EventBus::QUEUE_LENGTH; ++i)
        assert(bus.post(EventId::ConfigChanged, &i, sizeof(i)));
    auto publish = [&](DataKey key) {
        DataChangedPayload p{key};
        return bus.tryPost(EventId::DataChanged, &p, sizeof(p), ModuleId::DataStore);
    };
    assert(!bus.tryPost(EventId::SystemStarted));
    changes.tryDrain(4, publish);
    assert(eventbusWarnings.empty());
    bool startPending = true;
    for (unsigned pass = 0; pass < DataKeys::ReservedMax + 10U; ++pass) {
        bus.dispatch(16);
        if (startPending) startPending = !bus.tryPost(EventId::SystemStarted);
        if (!startPending) changes.tryDrain(4, publish);
    }
    assert(transitions == EventBus::QUEUE_LENGTH && started == 1);
    for (auto count : received) assert(count == 1);
    assert(eventbusWarnings.empty());
    // Repeat saturation after startup: runtime notifications must also retry.
    for (unsigned round = 0; round < 2; ++round) {
        for (unsigned i = 0; i < EventBus::QUEUE_LENGTH; ++i) {
            const unsigned value = transitions + i;
            assert(bus.post(EventId::ConfigChanged, &value, sizeof(value)));
        }
        for (DataKey key = 0; key <= DataKeys::ReservedMax; ++key) {
            assert(changes.mark(key));
            assert(changes.mark(key));
        }
        for (unsigned blocked = 0; blocked < 20; ++blocked) changes.tryDrain(4, publish);
        for (unsigned pass = 0; pass < DataKeys::ReservedMax + 10U; ++pass) {
            bus.dispatch(16);
            changes.tryDrain(4, publish);
        }
        for (auto count : received) assert(count == round + 2);
        assert(started == 1 && eventbusWarnings.empty());
    }
    auto snapshot = bus.queueSnapshot();
    assert(snapshot.peak == EventBus::QUEUE_LENGTH && snapshot.queued == 0);
    assert(snapshot.fullDrops == 0 && snapshot.fullRetries == 42);
    assert(snapshot.accepted == snapshot.dequeued);

    // A brief full queue is visible after draining, and retryable slots keep FIFO order.
    constexpr uint64_t sinceBootUs = uint64_t{UINT32_MAX} + 1U;
    fakeEspTimerUs = sinceBootUs;
    for (unsigned i = 0; i < EventBus::QUEUE_LENGTH; ++i) {
        const unsigned value = transitions + i;
        assert(bus.post(EventId::ConfigChanged, &value, sizeof(value), ModuleId::ConfigStore));
        ++fakeEspTimerUs;
    }
    assert(!bus.post(EventId::ConfigChanged));
    assert(!bus.postFromISR(EventId::AlarmRaised, nullptr, 0, ModuleId::Alarm));
    assert(!bus.tryPost(EventId::SystemStarted, nullptr, 1));
    assert(!bus.post(EventId::DataChanged, nullptr, EventBus::MAX_PAYLOAD_SIZE + 1));
    fakeEspTimerUs = sinceBootUs + 150U;
    snapshot = bus.queueSnapshot();
    assert(snapshot.queued == snapshot.capacity);
    assert(snapshot.fullDrops == 2 && snapshot.isrDrops == 1 && snapshot.controlDrops == 2);
    assert(snapshot.oversizeDrops == 1 && snapshot.missingPayloadDrops == 1);
    assert(snapshot.oldestEvent == EventId::ConfigChanged && snapshot.oldestProducer == ModuleId::ConfigStore);
#if FLOW_PERFORMANCE_DIAGNOSTICS
    assert(snapshot.oldestAgeUs == 150U && snapshot.maxQueueDelayUs == 0);
#else
    assert(snapshot.oldestAgeUs == 0 && snapshot.maxQueueDelayUs == 0);
#endif
    bus.dispatch(EventBus::QUEUE_LENGTH);
    snapshot = bus.queueSnapshot();
#if FLOW_PERFORMANCE_DIAGNOSTICS
    assert(snapshot.maxQueueDelayUs == 150U && snapshot.slowestEvent == EventId::ConfigChanged);
#else
    assert(snapshot.maxQueueDelayUs == 0 && snapshot.slowestEvent == EventId::None);
#endif
    assert(snapshot.oldestEvent == EventId::None && snapshot.oldestAgeUs == 0U);
    assert(snapshot.queued == 0 && snapshot.accepted == snapshot.dequeued);
    assert(eventbusWarnings.empty()); // Publication losses are reported by the Supervisor.
    assert(!bus.subscribe(EventId::SystemStarted, nullptr, nullptr));
    assert(bus.queueSnapshot().subscriptionDrops == 1);
    bus.dispatch(0);
    assert(eventbusWarnings.size() == 1);
    assert(eventbusWarnings.back().find("null_cb") != std::string::npos);

    testConcurrentProducers();
    puts("EventBus: FIFO, retry, exact peak, latency, diagnostics and concurrent producers OK");
}

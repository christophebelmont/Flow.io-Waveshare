#include "Core/EventBus/EventBus.h"
#include "Core/DataStore/StartupDataChanges.h"
#include "Core/Log.h"
#include <Arduino.h>
#include <cassert>

int main() {
    EventBus bus;
    StartupDataChanges changes;
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
    changes.drain(4, publish);
    assert(eventbusWarnings.empty());
    bool startPending = true;
    for (unsigned pass = 0; pass < DataKeys::ReservedMax + 10U; ++pass) {
        bus.dispatch(16);
        if (startPending) startPending = !bus.tryPost(EventId::SystemStarted);
        if (!startPending) changes.drain(4, publish);
    }
    assert(transitions == EventBus::QUEUE_LENGTH && started == 1);
    for (auto count : received) assert(count == 1);
    assert(eventbusWarnings.empty());
    // Actual unretained overflow must still count and report a loss.
    for (unsigned i = 0; i < EventBus::QUEUE_LENGTH; ++i) assert(bus.post(EventId::ConfigChanged));
    assert(!bus.post(EventId::ConfigChanged));
    assert(eventbusWarnings.size() == 1);
    assert(eventbusWarnings.back().find("drop_total=1") != std::string::npos);
    puts("EventBus startup: full queue retry without loss, all keys delivered, transition ordering, single SystemStarted, real loss accounting OK");
}

#pragma once
/**
 * @file DataStore.h
 * @brief Runtime data store with EventBus notifications.
 */
#include <stdint.h>
#include <string.h>

#include "Core/DataModel.h"
#include "Core/EventBus/EventBus.h"
#include "Core/EventBus/EventId.h"
#include "Core/EventBus/EventPayloads.h"

/**
 * @brief Stores runtime data and publishes changes on the EventBus.
 */
class DataStore {
public:
    /** @brief Construct an empty data store. */
    DataStore() = default;

    /** @brief Inject EventBus dependency for notifications. */
    void setEventBus(EventBus* bus) { _bus = bus; }

    /** @brief Safe read access to the full runtime model. */
    const RuntimeData& data() const { return _rt; }
    /** @brief Mutable access for module-owned setters. */
    RuntimeData& dataMutable() { return _rt; }

    /** @brief Notify a data key change. */
    void notifyChanged(DataKey key);

private:
    RuntimeData _rt{};
    EventBus* _bus = nullptr;

private:
    void publishChanged(DataKey key);
};

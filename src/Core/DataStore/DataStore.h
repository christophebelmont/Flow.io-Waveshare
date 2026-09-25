#pragma once
/**
 * @file DataStore.h
 * @brief Runtime data store with EventBus notifications.
 */
#include <stdint.h>
#include "StartupDataChanges.h"
#include <string.h>

#include "Core/DataModel.h"
#include "Core/EventBus/EventBus.h"
#include "Core/EventBus/EventId.h"
#include "Core/EventBus/EventPayloads.h"
#include "Core/Values/ValueRegistry.h"
#include "Core/DataKeys.h"

/**
 * @brief Stores runtime data and publishes changes on the EventBus.
 */
class DataStore {
public:
    /** @brief Construct an empty data store. */
    DataStore() = default;

    /** @brief Inject EventBus dependency for notifications. */
    void setEventBus(EventBus* bus) {
        _bus = bus;
        values.setNotification([](void* context, ValueId id, uint32_t) {
            auto* self = static_cast<DataStore*>(context);
            if (!self->_bus) return;
            self->notifyChanged(DataKeys::ValueBase + id);
        }, this);
    }

    ValueRegistry values;
    static_assert(ValueIds::Capacity <= DataKeys::ValueReservedCount, "Value DataKey capacity");

    /** @brief Safe read access to the full runtime model. */
    const RuntimeData& data() const { return _rt; }
    /** @brief Mutable access for module-owned setters. */
    RuntimeData& dataMutable() { return _rt; }

    /** @brief Notify a data key change. */
    void notifyChanged(DataKey key);
    /** Called only by the EventBus task after all config callbacks finished. */
    void flushStartupChanges(uint16_t budget);

private:
    StartupDataChanges startupChanges_;
    RuntimeData _rt{};
    EventBus* _bus = nullptr;

private:
    void publishChanged(DataKey key);
};

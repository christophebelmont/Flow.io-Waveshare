#pragma once
/**
 * @file DataStore.h
 * @brief Runtime data store with EventBus notifications.
 */
#include <stdint.h>
#include "PendingDataKeys.h"
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
    // Exposure policy is loaded once per boot, independently of value production.
    void setPublishHa(ValueId id, bool enabled) { if (id < ValueIds::Capacity) publishHa_[id] = enabled; }
    bool publishHa(ValueId id) const { return id < ValueIds::Capacity && publishHa_[id]; }
    void bindExposureConfig(ValueId id, const bool* ha, const bool* history, const bool* usable) {
        if (id < ValueIds::Capacity) { configuredHa_[id] = ha; configuredHistory_[id] = history; configuredUsable_[id] = usable; }
    }
    bool configuredPublishHa(ValueId id) const { return id < ValueIds::Capacity && configuredHa_[id] ? *configuredHa_[id] : publishHa(id); }
    bool configuredUsable(ValueId id) const { return id < ValueIds::Capacity && configuredUsable_[id] ? *configuredUsable_[id] : values.usable(id); }
    bool configuredHistory(ValueId id) const { return id < ValueIds::Capacity && configuredHistory_[id] ? *configuredHistory_[id] : true; }
    static_assert(ValueIds::Capacity <= DataKeys::ValueReservedCount, "Value DataKey capacity");

    /** @brief Safe read access to the full runtime model. */
    const RuntimeData& data() const { return _rt; }
    /** @brief Mutable access for module-owned setters. */
    RuntimeData& dataMutable() { return _rt; }

    /** @brief Notify a data key change. */
    void notifyChanged(DataKey key);
    /** Called only by the EventBus task after all config callbacks finished. */
    void flushPendingChanges(uint16_t budget);

private:
    bool publishHa_[ValueIds::Capacity]{};
    const bool* configuredHa_[ValueIds::Capacity]{};
    const bool* configuredHistory_[ValueIds::Capacity]{};
    const bool* configuredUsable_[ValueIds::Capacity]{};
    PendingDataKeys pendingChanges_;
    RuntimeData _rt{};
    EventBus* _bus = nullptr;

};

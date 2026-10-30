#pragma once
/**
 * @file DataStore.h
 * @brief Runtime data store with EventBus notifications.
 */
#include <stdint.h>
#include "PendingDataKeys.h"
#include <string.h>
#include <esp_heap_caps.h>
#include <new>

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
    ~DataStore() { if (exposure_) { exposure_->~ExposureStorage(); heap_caps_free(exposure_); } }
    DataStore(const DataStore&) = delete;
    DataStore& operator=(const DataStore&) = delete;

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
    using RegisterExposure = bool (*)(void*, ValueId);
    void setExposureRegistrar(RegisterExposure callback, void* context) { exposureRegistrar_ = callback; exposureContext_ = context; }
    bool defineDataValue(ValueId id, const ValueMetadata& metadata) {
        return exposureRegistrar_ && exposureRegistrar_(exposureContext_, id) && values.define(id, metadata);
    }
    void setPublishHa(ValueId id, bool enabled) { if (ensureExposure_() && id < ValueIds::Capacity) exposure_->publishHa[id] = enabled; }
    bool publishHa(ValueId id) const { return exposure_ && id < ValueIds::Capacity && exposure_->publishHa[id]; }
    bool bindExposureConfig(ValueId id, const bool* ha, const bool* history, const bool* usable) {
        if (ensureExposure_() && id < ValueIds::Capacity) { exposure_->ha[id] = ha; exposure_->history[id] = history; exposure_->usable[id] = usable; return true; }
        return false;
    }
    bool configuredPublishHa(ValueId id) const { return exposure_ && id < ValueIds::Capacity && exposure_->ha[id] ? *exposure_->ha[id] : publishHa(id); }
    bool configuredUsable(ValueId id) const { return exposure_ && id < ValueIds::Capacity && exposure_->usable[id] ? *exposure_->usable[id] : values.usable(id); }
    bool configuredHistory(ValueId id) const { return exposure_ && id < ValueIds::Capacity && exposure_->history[id] ? *exposure_->history[id] : values.history(id); }
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
    struct ExposureStorage {
        bool publishHa[ValueIds::Capacity]{};
        const bool* ha[ValueIds::Capacity]{};
        const bool* history[ValueIds::Capacity]{};
        const bool* usable[ValueIds::Capacity]{};
    };
    bool ensureExposure_() {
        if (exposure_) return true;
        void* memory = heap_caps_malloc(sizeof(ExposureStorage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory) exposure_ = new (memory) ExposureStorage{};
        return exposure_ != nullptr;
    }
    ExposureStorage* exposure_ = nullptr;
    RegisterExposure exposureRegistrar_ = nullptr;
    void* exposureContext_ = nullptr;
    PendingDataKeys pendingChanges_;
    RuntimeData _rt{};
    EventBus* _bus = nullptr;

};

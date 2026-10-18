/**
 * @file DataStore.cpp
 * @brief Implementation file.
 */
#include "Core/DataStore/DataStore.h"
#include "Core/ModuleId.h"
#include <esp_timer.h>

void DataStore::notifyChanged(DataKey key)
{
    pendingChanges_.mark(key);
}

void DataStore::flushPendingChanges(uint16_t budget)
{
    values.expire(uint64_t(esp_timer_get_time()) / 1000ULL);
    if (!_bus) return;
    pendingChanges_.tryDrain(budget, [this](DataKey key) {
        const DataChangedPayload payload{key};
        return _bus->tryPost(EventId::DataChanged, &payload, sizeof(payload), ModuleId::DataStore);
    });
}

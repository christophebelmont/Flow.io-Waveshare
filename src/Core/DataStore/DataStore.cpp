/**
 * @file DataStore.cpp
 * @brief Implementation file.
 */
#include "Core/DataStore/DataStore.h"
#include "Core/ModuleId.h"

void DataStore::publishChanged(DataKey key)
{
    if (!_bus) return;
    DataChangedPayload p{ key };
    _bus->post(EventId::DataChanged, &p, sizeof(p), ModuleId::DataStore);
}

void DataStore::notifyChanged(DataKey key)
{
    if (!startupChanges_.mark(key)) publishChanged(key);
}

void DataStore::flushStartupChanges(uint16_t budget)
{
    if (!_bus) return;
    startupChanges_.drain(budget, [this](DataKey key) {
        const DataChangedPayload payload{key};
        return _bus->tryPost(EventId::DataChanged, &payload, sizeof(payload), ModuleId::DataStore);
    });
}

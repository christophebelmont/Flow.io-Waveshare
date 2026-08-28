/**
 * @file DataStoreModule.cpp
 * @brief Implementation file.
 */
#include "DataStoreModule.h"
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::DataStoreModule)
#include "Core/ModuleLog.h"

void DataStoreModule::init(ConfigStore&, ServiceRegistry& services)
{
    auto* eb = services.get<EventBusService>(ServiceId::EventBus);
    if (eb && eb->bus) {
        _store.setEventBus(eb->bus);
    }

    if (!services.add(ServiceId::DataStore, &_svc)) {
        LOGE("service registration failed: %s", toString(ServiceId::DataStore));
    }
}

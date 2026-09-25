/**
 * @file DataStoreModule.cpp
 * @brief Implementation file.
 */
#include "DataStoreModule.h"
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::DataStoreModule)
#include "Core/ModuleLog.h"

void DataStoreModule::init(ConfigStore&, ServiceRegistry& services)
{
    if (!_store.values.begin()) {
        LOGE("Value storage allocation failed bytes=%u", (unsigned)ValueRegistry::storageBytes());
    } else {
        LOGI("Value storage ready bytes=%u memory=%s", (unsigned)ValueRegistry::storageBytes(),
             _store.values.storageInPsram() ? "psram" : "internal");
    }
    auto* eb = services.get<EventBusService>(ServiceId::EventBus);
    if (eb && eb->bus) {
        _store.setEventBus(eb->bus);
    }

    if (!services.add(ServiceId::DataStore, &_svc)) {
        LOGE("service registration failed: %s", toString(ServiceId::DataStore));
    }
}

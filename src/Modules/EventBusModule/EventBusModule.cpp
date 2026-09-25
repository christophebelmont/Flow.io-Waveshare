/**
 * @file EventBusModule.cpp
 * @brief Implementation file.
 */
#include "EventBusModule.h"
#include "Core/ModuleId.h"
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::EventBusModule)
#include "Core/ModuleLog.h"


void EventBusModule::init(ConfigStore&, ServiceRegistry& services) {
    /// récupérer service loghub (log async)
    logHub = services.get<LogHubService>(ServiceId::LogHub);

    if (!services.add(ServiceId::EventBus, &_svc)) {
        LOGE("service registration failed: %s", toString(ServiceId::EventBus));
    }

    LOGI("EventBusService registered");
}

void EventBusModule::onStart(ConfigStore&, ServiceRegistry& services)
{
    const auto* dataStore = services.get<DataStoreService>(ServiceId::DataStore);
    dataStore_ = dataStore ? dataStore->store : nullptr;
}

void EventBusModule::loop() {
    /// Dispatch queued events.
    _bus.dispatch(16);
    // Never discard startup notification when ordinary events fill the queue.
    if (systemStartedPending_) {
        systemStartedPending_ = !_bus.tryPost(EventId::SystemStarted, nullptr, 0, ModuleId::EventBus);
    }
    if (!systemStartedPending_ && dataStore_) dataStore_->flushStartupChanges(4);
    vTaskDelay(pdMS_TO_TICKS(5));
}

/**
 * @file LogDispatcherModule.cpp
 * @brief Implementation file.
 */
#include "LogDispatcherModule.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

ModuleTaskSpec LogDispatcherModule::taskSpec(uint8_t index) const {
    if (index != 0U || taskCount() == 0U) return {};
    return {
        "LogDispatch",
        4096,
        3,
        1,
        &LogDispatcherModule::taskFn,
        const_cast<LogDispatcherModule*>(this),
        TaskRole::Primary,
        {TaskCadence::EventDriven}
    };
}

void LogDispatcherModule::init(ConfigStore& cfg, ServiceRegistry& services) {
    (void)cfg;

    _services = &services;

    /// récupérer hub et sink registry
    auto hubSvc = services.get<LogHubService>(ServiceId::LogHub);
    _sinkReg = services.get<LogSinkRegistryService>(ServiceId::LogSinks);

    /// hub object dans ctx => on va le passer en brut via ctx->hub
    /// On va récupérer le LogHub* via hubSvc->ctx
    if (!hubSvc || !hubSvc->ctx || !_sinkReg) return;

    _hub = static_cast<LogHub*>(hubSvc->ctx);

}

void LogDispatcherModule::taskFn(void* pv, TaskHealth& health) {
    auto& self = *static_cast<LogDispatcherModule*>(pv);
    const auto* sinks = self._sinkReg;
    LogEntry e;

    while (true) {
        if (self._hub->dequeue(e, portMAX_DELAY)) {
            health.beginCycle();
            int n = sinks->count(sinks->ctx);

            for (int i = 0; i < n; ++i) {
                LogSinkService sink = sinks->get(sinks->ctx, i);
                if (sink.write) sink.write(sink.ctx, e);
            }
            health.endCycle();
        }
    }
}

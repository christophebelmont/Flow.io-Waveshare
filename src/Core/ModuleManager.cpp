/**
 * @file ModuleManager.cpp
 * @brief Implementation file.
 */
#include "ModuleManager.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <new>
#include "Core/Log.h"
#include "Core/LogModuleIds.h"
#include "Core/Services/ISystemHealth.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::CoreModuleManager)

namespace {
constexpr uint8_t kStartupPreparedFlag = 0x01U;
constexpr uint8_t kStartupActiveFlag = 0x02U;
constexpr uint8_t kStartupFailedFlag = 0x04U;

const char* stackCapsLabel_(UBaseType_t caps) {
    if ((caps & MALLOC_CAP_SPIRAM) != 0U) return "SPIRAM";
    if ((caps & MALLOC_CAP_INTERNAL) != 0U) return "INTERNAL";
    return "DEFAULT";
}
}

static void logRegisteredModules(Module* modules[], uint8_t count) {
    ///Logger::log(LogLevel::Info, "MOD", "Registered modules (%u):", count);
    for (uint8_t i = 0; i < count; ++i) {
        ///Logger::log(LogLevel::Info, "MOD", " - %s", toString(modules[i]->moduleId()));
    }
}

static void dbgDumpModules(Module* modules[], uint8_t count) {
    Log::debug(LOG_MODULE_ID, "registered modules count=%u", (unsigned)count);
    for (uint8_t i = 0; i < count; ++i) {
        if (!modules[i]) continue;
        Log::debug(LOG_MODULE_ID, "module=%s deps=%u",
                   toString(modules[i]->moduleId()),
                   (unsigned)modules[i]->dependencyCount());
        for (uint8_t d = 0; d < modules[i]->dependencyCount(); ++d) {
            const ModuleId dep = modules[i]->dependency(d);
            Log::debug(LOG_MODULE_ID, "module=%s needs=%s",
                       toString(modules[i]->moduleId()),
                       toString(dep));
        }
    }
}

static bool isValidTaskSpec(const ModuleTaskSpec& spec) {
    return spec.name && spec.name[0] != '\0' && spec.stackSize > 0 && spec.entry != nullptr;
}

bool ModuleManager::add(Module* m) {
    if (!m) {
        Log::error(LOG_MODULE_ID, "add failed: null module");
        return false;
    }

    const ModuleId moduleId = m->moduleId();
    if (!isValidModuleId(moduleId)) {
        Log::error(LOG_MODULE_ID, "add failed: invalid module id");
        return false;
    }

    if (count >= Limits::Core::Capacity::MaxModules) {
        Log::error(LOG_MODULE_ID, "add failed: module limit reached (%u)",
                   (unsigned)Limits::Core::Capacity::MaxModules);
        return false;
    }

    if (findById(moduleId)) {
        Log::error(LOG_MODULE_ID, "add failed: duplicate module id=%s", toString(moduleId));
        return false;
    }

    modules[count++] = m;
    modulesById[moduleIdIndex(moduleId)] = m;
    return true;
}

Module* ModuleManager::findById(ModuleId id) {
    if (!isValidModuleId(id)) return nullptr;
    return modulesById[moduleIdIndex(id)];
}

bool ModuleManager::buildInitOrder() {
    Log::debug(LOG_MODULE_ID, "buildInitOrder: count=%u", (unsigned)count);
    /// Kahn topo-sort
    bool placed[Limits::Core::Capacity::MaxModules] = {0};
    orderedCount = 0;

    for (uint8_t pass = 0; pass < count; ++pass) {
        bool progress = false;

        for (uint8_t i = 0; i < count; ++i) {
            Module* m = modules[i];
            if (!m || placed[i]) continue;

            /// Check if all dependencies are already placed
            bool depsOk = true;
            const uint8_t depCount = m->dependencyCount();

            for (uint8_t d = 0; d < depCount; ++d) {
                const ModuleId depId = m->dependency(d);
                if (!isValidModuleId(depId)) continue;

                Module* dep = findById(depId);
                if (!dep) {
                    Log::error(LOG_MODULE_ID, "missing dependency: module=%s requires=%s",
                               toString(m->moduleId()), toString(depId));
                    return false;
                }

                /// Is dep already placed in ordered[] ?
                bool depPlaced = false;
                for (uint8_t j = 0; j < count; ++j) {
                    if (modules[j] == dep) {
                        depPlaced = placed[j];
                        break;
                    }
                }

                if (!depPlaced) {
                    depsOk = false;
                    break;
                }
            }

            if (depsOk) {
                ordered[orderedCount++] = m;
                placed[i] = true;
                progress = true;
            }
        }
        
        if (orderedCount == count) {
            return true;
        }

        if (!progress) {
            Log::error(LOG_MODULE_ID, "cyclic or unresolved deps detected");
            for (uint8_t i = 0; i < count; ++i) {
                if (modules[i] && !placed[i]) {
                    Log::error(LOG_MODULE_ID, "not placed module=%s", toString(modules[i]->moduleId()));
                }
            }
            return false;
        }
    }

    Log::debug(LOG_MODULE_ID, "buildInitOrder: success (ordered=%u)", (unsigned)orderedCount);
    return true;
}


bool ModuleManager::initAll(ConfigStore& cfg, ServiceRegistry& services) {
    Log::debug(LOG_MODULE_ID, "initAll: moduleCount=%u", (unsigned)count);
    /*Serial.printf("[MOD] moduleCount=%d\n", count);
    for (int i=0;i<count;i++){
        Serial.printf("[MOD] module[%d]=%s\n", i, toString(modules[i]->moduleId()));
    }
    Serial.flush();
    delay(20);*/

    // dbgDumpModules(modules, count); // debug dump

    if (!buildInitOrder()) return false;

    for (uint8_t i = 0; i < orderedCount; ++i) {
        const ModuleId moduleId = ordered[i]->moduleId();
        const LogModuleId logModuleId = logModuleIdFromModuleId(moduleId);
        if (logModuleId != (LogModuleId)LogModuleIdValue::Unknown) {
            (void)Log::registerModule(logModuleId, toString(moduleId));
        }
        ///Logger::log(LogLevel::Info, "MOD", "Init %s", ordered[i]->moduleId());
        Log::debug(LOG_MODULE_ID, "init: %s", toString(moduleId));
        ordered[i]->init(cfg, services);
        if (logModuleId != (LogModuleId)LogModuleIdValue::Unknown) {
            (void)Log::registerModule(logModuleId, toString(moduleId));
        }
    }

    // Core shared dependencies must be injected before config callbacks and
    // before any module task can start mutating shared runtime state.
    wireCoreServices(services, cfg);

    /// Load persistent config after all modules registered their variables.
    cfg.loadPersistent();

    for (uint8_t i = 0; i < orderedCount; ++i) ordered[i]->onConfigPrepared(cfg, services);

    for (uint8_t i = 0; i < orderedCount; ++i) {
        ordered[i]->onConfigLoaded(cfg, services);
    }

    if (!declareTasks_()) return false;

    startupEpochMs_ = millis();
    startupFlags_ = (uint8_t)(kStartupPreparedFlag | kStartupActiveFlag);
    startupStartedMask_ = 0U;

    if (!tickStartup(cfg, services)) {
        return false;
    }

    Log::debug(LOG_MODULE_ID, "initAll: startup prepared modules=%u", (unsigned)orderedCount);
    Log::debug(LOG_MODULE_ID, "initAll: done");
    
    return true;
}

bool ModuleManager::dependenciesStarted_(Module& module) const
{
    const uint8_t depCount = module.dependencyCount();
    for (uint8_t d = 0; d < depCount; ++d) {
        const ModuleId depId = module.dependency(d);
        if (!isValidModuleId(depId)) continue;

        bool found = false;
        for (uint8_t i = 0; i < orderedCount; ++i) {
            Module* dep = ordered[i];
            if (!dep || dep->moduleId() != depId) continue;
            found = true;
            if ((startupStartedMask_ & (1UL << i)) == 0U) return false;
            break;
        }
        if (!found) return false;
    }
    return true;
}

bool ModuleManager::startModule_(uint8_t orderedIdx, ConfigStore& cfg, ServiceRegistry& services)
{
    if (orderedIdx >= orderedCount) return false;
    if ((startupStartedMask_ & (1UL << orderedIdx)) != 0U) return true;

    Module* module = ordered[orderedIdx];
    if (!module) return false;
    if (!dependenciesStarted_(*module)) return true;
    if (!module->canStart(cfg, services)) return true;

    module->onStart(cfg, services);

    uint8_t startedTasks = 0;
    for (uint8_t i = 0; i < taskEntryCount; ++i) {
        TaskEntry& entry = taskEntries[i];
        if (entry.module != module || entry.deferred) continue;
        if (!startTask_(entry)) return false;
        ++startedTasks;
    }

    startupStartedMask_ |= (1UL << orderedIdx);
    Log::debug(LOG_MODULE_ID, "startup release: module=%s delay_ms=%lu tasks=%u",
               toString(module->moduleId()),
               (unsigned long)module->startDelayMs(),
               (unsigned)startedTasks);
    return true;
}

void ModuleManager::attachArduinoLoop(const char* name, uint32_t stackBytes, BaseType_t core)
{
    TaskEntry& entry = taskEntries[0];
    entry.spec = {name, stackBytes, 1, core, nullptr, nullptr, TaskRole::ArduinoLoop};
    entry.id = taskHealthId(ModuleId::Unknown, TaskRole::ArduinoLoop);
    entry.health.configure(entry.spec.timing);
    entry.health.requestStart();
    entry.health.started();
    taskEntryCount = 1;
}

bool ModuleManager::declareTasks_()
{
    for (uint8_t i = 0; i < orderedCount; ++i) {
        Module& module = *ordered[i];
        module.manager_ = this;
        const uint8_t count = module.taskCount();
        for (uint8_t index = 0; index < count; ++index) {
            if (!declareTask_(module, module.taskSpec(index), false)) return false;
        }
        const auto deferred = module.deferredTaskSpec();
        if (deferred.entry && !declareTask_(module, deferred, true)) return false;
    }
    return true;
}

bool ModuleManager::declareDisabledTasks(Module& module)
{
    const uint8_t count = module.taskCount();
    for (uint8_t i = 0; i < count; ++i) {
        if (!declareTask_(module, module.taskSpec(i), false, TaskHealthState::Disabled)) return false;
    }
    return true;
}

bool ModuleManager::declareTask_(Module& module, const ModuleTaskSpec& spec, bool deferred,
                                TaskHealthState initialState)
{
    const uint16_t id = taskHealthId(module.moduleId(), spec.role);
    if (!isValidTaskSpec(spec) || spec.minStackFreeBytes == 0U ||
        spec.minStackFreeBytes > spec.stackSize ||
        taskEntryCount >= Limits::Core::Capacity::MaxTaskHealthEntries) {
        Log::error(LOG_MODULE_ID, "invalid task spec or full registry id=%u count=%u",
                   (unsigned)id, (unsigned)taskEntryCount);
        return false;
    }
    for (uint8_t i = 0; i < taskEntryCount; ++i) {
        if (taskEntries[i].id == id) {
            Log::error(LOG_MODULE_ID, "duplicate task id=%u", (unsigned)id);
            return false;
        }
    }

    TaskEntry& entry = taskEntries[taskEntryCount++];
    entry.spec = spec;
    entry.module = &module;
    entry.owner = this;
    entry.id = id;
    entry.deferred = deferred;
    entry.stackCaps = module.taskStackCaps();
#if configSUPPORT_STATIC_ALLOCATION
    entry.withCaps = (entry.stackCaps & MALLOC_CAP_SPIRAM) != 0U ||
                     (entry.stackCaps & MALLOC_CAP_INTERNAL) == 0U;
#endif
#if FLOW_PERFORMANCE_DIAGNOSTICS
    TaskWakeTrace* wakeTrace = nullptr;
    TaskWakeRuntimeTrace* runtimeTrace = nullptr;
    if (spec.traceWake && initialState == TaskHealthState::Expected) {
        void* storage = heap_caps_malloc(sizeof(TaskWakeTrace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (storage) {
            wakeTrace = new (storage) TaskWakeTrace{};
            Log::info(LOG_MODULE_ID, "TaskWake trace id=%u capacity=%u psram_b=%lu",
                      (unsigned)id, (unsigned)TaskWakeTrace::kCapacity, (unsigned long)sizeof(TaskWakeTrace));
        } else {
            Log::warn(LOG_MODULE_ID, "TaskWake trace unavailable id=%u: PSRAM allocation failed", (unsigned)id);
        }
    }
    if (wakeTrace && spec.traceWakeRuntime && wakeRuntimeReader_.read) {
        void* storage = heap_caps_malloc(sizeof(TaskWakeRuntimeTrace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (storage) {
            runtimeTrace = new (storage) TaskWakeRuntimeTrace{};
            runtimeTrace->reader = wakeRuntimeReader_;
            Log::info(LOG_MODULE_ID, "TaskWake CPU id=%u psram_b=%lu", (unsigned)id,
                      (unsigned long)sizeof(TaskWakeRuntimeTrace));
        } else {
            Log::warn(LOG_MODULE_ID, "TaskWake CPU unavailable id=%u: PSRAM allocation failed", (unsigned)id);
        }
    }
    entry.health.configure(spec.timing, initialState, wakeTrace, runtimeTrace);
#else
    entry.health.configure(spec.timing, initialState);
#endif
    return true;
}

bool ModuleManager::startTask_(TaskEntry& entry)
{
    if (!entry.health.requestStart()) return false;
    const auto& spec = entry.spec;
    TaskHandle_t handle = nullptr;
    BaseType_t result;
#if configSUPPORT_STATIC_ALLOCATION
    if (entry.withCaps) {
        result = xTaskCreatePinnedToCoreWithCaps(&ModuleManager::taskEntry_, spec.name, spec.stackSize,
                                                &entry, spec.priority, &handle, spec.coreId, entry.stackCaps);
    } else
#endif
    {
        result = xTaskCreatePinnedToCore(&ModuleManager::taskEntry_, spec.name, spec.stackSize,
                                        &entry, spec.priority, &handle, spec.coreId);
    }

    const auto level = result == pdPASS && handle ? LogLevel::Info : LogLevel::Error;
    if (level == LogLevel::Error) entry.health.createFailed();

    Log::logf(level, LOG_MODULE_ID,
              "startTask id=%u task=%s err=%ld stack=%lu caps=%s internal=%lu largest=%lu",
              (unsigned)entry.id, spec.name, (long)result, (unsigned long)spec.stackSize,
              stackCapsLabel_(entry.stackCaps),
              (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return level == LogLevel::Info;
}

void ModuleManager::taskEntry_(void* context)
{
    auto& entry = *static_cast<TaskEntry*>(context);
    entry.health.started();
    if (entry.spec.role == TaskRole::Primary) {
        entry.module->primaryTaskHandle_.store(xTaskGetCurrentTaskHandle());
    }
    entry.spec.entry(entry.spec.context, entry.health);
    entry.health.finish();
    entry.owner->reapRequested_.store(true);
    // The owner frees the task only once it has entered suspension.
    for (;;) vTaskSuspend(nullptr);
}

void ModuleManager::reapFinishedTasks()
{
    if (!reapRequested_.exchange(false)) return;

    for (uint8_t i = 0; i < taskEntryCount; ++i) {
        TaskEntry& entry = taskEntries[i];
        const auto health = entry.health.snapshot();
        if (health.state != TaskHealthState::Finishing) continue;
        if (!entry.health.canReap() || eTaskGetState(health.handle) != eSuspended) {
            reapRequested_.store(true);
            continue;
        }
        if (entry.spec.role == TaskRole::Primary) entry.module->primaryTaskHandle_.store(nullptr);
#if configSUPPORT_STATIC_ALLOCATION
        if (entry.withCaps) {
            vTaskDeleteWithCaps(health.handle);
        } else
#endif
        {
            vTaskDelete(health.handle);
        }
        entry.health.reaped();
        Log::info(LOG_MODULE_ID, "task finished id=%u name=%s free_min=%luB",
                  (unsigned)entry.id, entry.spec.name, (unsigned long)health.minFreeBytes);
    }
}

bool ModuleManager::startDeferredTask_(Module& module)
{
    for (uint8_t i = 0; i < taskEntryCount; ++i) {
        TaskEntry& entry = taskEntries[i];
        if (entry.module == &module && entry.deferred) return startTask_(entry);
    }
    return false;
}

bool Module::startDeferredTask()
{
    return manager_ && manager_->startDeferredTask_(*this);
}

bool ModuleManager::tickStartup(ConfigStore& cfg, ServiceRegistry& services)
{
    if ((startupFlags_ & kStartupPreparedFlag) == 0U) return true;
    if ((startupFlags_ & kStartupFailedFlag) != 0U) return false;
    if ((startupFlags_ & kStartupActiveFlag) == 0U) return true;

    const uint32_t elapsedMs = millis() - startupEpochMs_;
    bool allStarted = true;
    for (uint8_t i = 0; i < orderedCount; ++i) {
        if ((startupStartedMask_ & (1UL << i)) != 0U) continue;
        Module* module = ordered[i];
        if (!module) continue;

        const uint32_t releaseDelayMs = module->startDelayMs();
        if (elapsedMs < releaseDelayMs) {
            allStarted = false;
            continue;
        }

        const bool wasStarted = (startupStartedMask_ & (1UL << i)) != 0U;
        if (!startModule_(i, cfg, services)) {
            startupFlags_ |= kStartupFailedFlag;
            startupFlags_ &= (uint8_t)~kStartupActiveFlag;
            return false;
        }
        const bool nowStarted = (startupStartedMask_ & (1UL << i)) != 0U;
        if (!nowStarted) {
            allStarted = false;
        }
    }

    if (allStarted) {
        startupFlags_ &= (uint8_t)~kStartupActiveFlag;
    }
    return true;
}

void ModuleManager::wireCoreServices(ServiceRegistry& services, ConfigStore& config) {
#if FLOW_PERFORMANCE_DIAGNOSTICS
  if (const auto* health = services.get<SystemHealthService>(ServiceId::SystemHealth)) {
      wakeRuntimeReader_ = health->wakeRuntimeReader;
  }
#endif

  auto* ebService = services.get<EventBusService>(ServiceId::EventBus);
  if (ebService && ebService->bus) {
    config.setEventBus(ebService->bus);
    Log::debug(LOG_MODULE_ID, "wireCoreServices: eventbus wired");
  }
}

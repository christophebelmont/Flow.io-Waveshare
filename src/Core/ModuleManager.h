#pragma once
/**
 * @file ModuleManager.h
 * @brief Dependency ordering and initialization for modules.
 */
#include "Module.h"

/**
 * @brief Registers modules, resolves dependencies, and orchestrates startup.
 *
 * Lifecycle driven by `ModuleManager`:
 *
 * 1. `init()`
 * 2. `ConfigStore::loadPersistent()`
 * 3. `onConfigPrepared()` declares configuration-dependent sources
 * 4. `onConfigLoaded()` resolves consumers after all declarations
 * 5. non-blocking startup sequencing:
 *    - wait until `startDelayMs()` has elapsed relative to the startup epoch
 *    - ensure every declared dependency has already been released
 *    - call `onStart()`
 *    - create the module tasks, if any
 *
 * This allows both active modules and passive modules to defer their business
 * startup work without blocking the global boot path.
 *
 * Current explicit startup delays:
 *
 * | Module | Delay (ms) | Rationale |
 * | --- | ---: | --- |
 * | `eventbus` | `0` | Event bus available immediately; publishes `SystemStarted` from its dispatch task once subscriptions are in place |
 * | `mqtt` | `1500` | Preserves the historical staged release before MQTT connect attempts |
 * | `poollogic` | `10000` | Preserves the historical delayed control-loop start |
 * | `ha` | `15000` | Preserves the historical delayed Home Assistant discovery startup |
 * | `webinterface` | `10000` | Preserves the web-server warm-up, now handled by `ModuleManager` |
 * | others | `0` | Immediate release after `onConfigLoaded()` unless the module overrides `startDelayMs()` |
 */
class ModuleManager {
public:
    /** @brief Add a module to the manager. */
    bool add(Module* m);
    /** @brief Declare the health row of a module disabled by the firmware profile. */
    bool declareDisabledTasks(Module& module);
    /** @brief Initialize modules, load config, and prepare the startup sequencer. */
    bool initAll(ConfigStore& cfg, ServiceRegistry& services);
    /** @brief Advance the non-blocking startup sequencer and release due modules. */
    bool tickStartup(ConfigStore& cfg, ServiceRegistry& services);
    /** @brief Whether every registered module has been released by the startup sequencer. */
    bool startupComplete() const { return (startupFlags_ & 0x01U) != 0U && (startupFlags_ & 0x06U) == 0U; }
    /** @brief Wire core services into the registry. */
    void wireCoreServices(ServiceRegistry& services, ConfigStore& config);
    
    /** @brief Immutable task declaration with an independently synchronized health slot. */
    struct TaskEntry {
        ModuleTaskSpec spec{};
        Module* module = nullptr;
        ModuleManager* owner = nullptr;
        UBaseType_t stackCaps = 0;
        uint16_t id = 0;
        bool withCaps = false;
        bool deferred = false;
        mutable TaskHealth health;
    };

    /** @brief Current module count. */
    uint8_t getCount() const { return count; }
    /** @brief Get a module by index. */
    Module* getModule(uint8_t idx) const {
        if (idx >= count) return nullptr;
        return modules[idx];
    }
    /** @brief Includes declared but not yet running tasks and the Arduino loop. */
    uint8_t getTaskEntryCount() const { return taskEntryCount; }
    const TaskEntry& getTaskEntry(uint8_t idx) const { return taskEntries[idx]; }
    void attachArduinoLoop(const char* name, uint32_t stackBytes, BaseType_t core);
    TaskHealth& arduinoLoopHealth() { return taskEntries[0].health; }
    void reapFinishedTasks();

private:
    friend class Module;
    Module* modules[Limits::Core::Capacity::MaxModules]{};
    Module* modulesById[kModuleIdCount]{};
    uint8_t count = 0;

    Module* ordered[Limits::Core::Capacity::MaxModules]{};
    uint8_t orderedCount = 0;
    TaskEntry taskEntries[Limits::Core::Capacity::MaxTaskHealthEntries]{};
    uint8_t taskEntryCount = 0;
    std::atomic<bool> reapRequested_{false};
    uint32_t startupEpochMs_ = 0;
    uint32_t startupStartedMask_ = 0;
    uint8_t startupFlags_ = 0; // bit0: prepared, bit1: active, bit2: failed

    Module* findById(ModuleId id);
    bool buildInitOrder();
    bool dependenciesStarted_(Module& module) const;
    bool startModule_(uint8_t orderedIdx, ConfigStore& cfg, ServiceRegistry& services);
    bool declareTasks_();
    bool declareTask_(Module& module, const ModuleTaskSpec& spec, bool deferred,
                      TaskHealthState initialState = TaskHealthState::Expected);
    bool startTask_(TaskEntry& entry);
    bool startDeferredTask_(Module& module);
    static void taskEntry_(void* context);
};

#pragma once
/**
 * @file Module.h
 * @brief Base interface for all runtime modules.
 */
#include "ConfigStore.h"
#include "Core/ModuleId.h"
#include "Core/SystemLimits.h"
#include "Runtime.h"
#include "ServiceRegistry.h"
#include "TaskHealth.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

/**
 * @brief Base class for active modules backed by a FreeRTOS task.
 */
struct ModuleTaskSpec {
    const char* name;
    uint32_t stackSize;
    UBaseType_t priority;
    BaseType_t coreId;
    void (*entry)(void*, TaskHealth&);
    void* context;
    TaskRole role = TaskRole::Primary;
    TaskTimingContract timing{};
    uint16_t minStackFreeBytes = kDefaultTaskMinFreeBytes;
    bool traceWake = false;
    bool traceWakeRuntime = false;
};

class Module {
public:
    /** @brief Virtual destructor. */
    virtual ~Module() = default;

    /** @brief Unique module identifier (used for dependency wiring). */
    virtual ModuleId moduleId() const = 0;
    /** @brief FreeRTOS task name for this module. */
    virtual const char* taskName() const = 0;

    /** @brief Number of declared dependencies. */
    virtual uint8_t dependencyCount() const { return 0; }
    /** @brief Dependency id at index, or ModuleId::Unknown if none. */
    virtual ModuleId dependency(uint8_t) const { return ModuleId::Unknown; }

    /** @brief Initialize module and register services/config. */
    virtual void init(ConfigStore& cfg, ServiceRegistry& services) = 0;
    /** @brief Called once all persistent config values are loaded. */
    // Declare configuration-dependent sources before consumers compile expressions.
    virtual void onConfigPrepared(ConfigStore&, ServiceRegistry&) {}
    virtual void onConfigLoaded(ConfigStore&, ServiceRegistry&) {}
    /** @brief Return whether the startup sequencer may release the module now. */
    virtual bool canStart(ConfigStore&, ServiceRegistry&) { return true; }
    /** @brief Called when the startup sequencer releases the module. */
    virtual void onStart(ConfigStore&, ServiceRegistry&) {}
    /** @brief Main module loop called from the module task. */
    virtual void loop() = 0;

    /** @brief Number of declared FreeRTOS tasks owned by this module. */
    virtual uint8_t taskCount() const { return 0; }
    /** @brief Descriptor of a declared task. The default runs loop() in task 0. */
    virtual ModuleTaskSpec taskSpec(uint8_t index) const {
        return index == 0U && taskCount() != 0U ? singleLoopTaskSpec() : ModuleTaskSpec{};
    }
    virtual ModuleTaskSpec deferredTaskSpec() const { return {}; }
    virtual TaskStackSource externalTaskStackSource() { return {}; }

    /** @brief Stack size for the FreeRTOS task. */
    virtual uint16_t taskStackSize() const { return Limits::Core::Task::DefaultStackSize; }
    /** @brief Task priority for the FreeRTOS task. */
    virtual UBaseType_t taskPriority() const { return 1; }
    /** @brief Memory capability flags used for the task stack allocation. */
    virtual UBaseType_t taskStackCaps() const { return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT; }
    /** @brief CPU core affinity for the FreeRTOS task (`0` or `1` on ESP32). */
    virtual BaseType_t taskCore() const { return 1; }
    /** @brief Relative startup delay in ms applied by `ModuleManager` before `onStart`. */
    virtual uint32_t startDelayMs() const { return 0U; }

    /** @brief Get the first started FreeRTOS task handle for this module. */
    TaskHandle_t getTaskHandle() const { return primaryTaskHandle_.load(); }

    /** @brief Whether this module owns at least one declared task. */
    virtual bool hasTask() const { return taskCount() > 0; }

protected:
    virtual TaskTimingContract taskTiming() const { return {}; }
    virtual uint16_t taskMinStackFreeBytes() const { return kDefaultTaskMinFreeBytes; }

    bool startDeferredTask();

    // Called by a primary loop when its work is complete. The framework's
    // cooperative delay still follows loop(), after this module-specific wait.
    void delayNextCycle(TickType_t ticks) {
        if (loopHealth_) {
            loopHealth_->endCycle(ticks + loopDelayTicks(), ticks);
            loopRecorded_ = true;
        }
        if (loopHealth_) loopHealth_->recordDelayStart(TaskWaitStage::Module, ticks);
        vTaskDelay(ticks);
        if (loopHealth_) loopHealth_->recordModuleDelayReturn();
    }

    // Event-driven loops mark the start after successfully receiving work.
    void beginTaskWork() {
        if (loopHealth_) loopHealth_->beginCycle();
    }

    void finishLoopTask() { loopFinishRequested_ = true; }

    /** @brief Helper for classic "loop() in one task" modules. */
    ModuleTaskSpec singleLoopTaskSpec() const {
        return {
            taskName(),
            (uint32_t)taskStackSize(),
            taskPriority(),
            taskCore(),
            &Module::taskEntry,
            const_cast<Module*>(this),
            TaskRole::Primary,
            taskTiming(),
            taskMinStackFreeBytes()
        };
    }

private:
    friend class ModuleManager;

    static constexpr TickType_t loopDelayTicks() {
        return pdMS_TO_TICKS(Limits::Core::Timing::LoopDelayMs);
    }

    static void taskEntry(void* arg, TaskHealth& health) {
        auto& self = *static_cast<Module*>(arg);
        self.loopHealth_ = &health;
        self.loopFinishRequested_ = false;
        const TaskCadence cadence = health.cadence();
        while (!self.loopFinishRequested_) {
            self.loopRecorded_ = false;
            if (cadence == TaskCadence::Periodic) health.beginCycle();
            self.loop();
            if (!self.loopRecorded_) health.endCycle(loopDelayTicks());
            if (!self.loopFinishRequested_) {
                health.recordDelayStart(TaskWaitStage::Loop, loopDelayTicks());
                vTaskDelay(loopDelayTicks());
                health.recordDelayReturn(TaskWaitStage::Loop);
            }
        }
        self.loopHealth_ = nullptr;
    }

    std::atomic<TaskHandle_t> primaryTaskHandle_{nullptr};
    class ModuleManager* manager_ = nullptr;
    TaskHealth* loopHealth_ = nullptr;
    bool loopFinishRequested_ = false;
    bool loopRecorded_ = false;
};

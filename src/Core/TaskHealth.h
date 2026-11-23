#pragma once

#include <stdint.h>
#include "Core/PerformanceDiagnostics.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Core/ModuleId.h"

enum class TaskRole : uint8_t { Primary = 0, Rs485 = 1, Reboot = 2, ArduinoLoop = 3, Library = 4, IdleCore1 = 5 };
enum class TaskCadence : uint8_t { Periodic, EventDriven, OneShot };
enum class TaskHealthState : uint8_t { Expected, Starting, Running, Finishing, Finished, CreateFailed, Disabled };

inline constexpr uint16_t kDefaultTaskMinFreeBytes = 512;

constexpr uint16_t taskHealthId(ModuleId module, TaskRole role)
{
    return (static_cast<uint16_t>(moduleIdIndex(module)) << 8U) | static_cast<uint8_t>(role);
}

struct TaskTimingContract {
    TaskCadence cadence = TaskCadence::Periodic;
    uint32_t workBudgetUs = 0;
    uint32_t wakeToleranceUs = 2000;
};

struct TaskTiming {
    enum class Phase : uint8_t { Idle, Working, Waiting };

    uint64_t workStartedUs = 0;
    uint64_t lastProgressUs = 0;
    uint64_t maxWorkUs = 0;
    uint64_t maxCycleUs = 0;
    uint32_t expectedWaitUs = 0;
    uint32_t cycles = 0;
    uint32_t lateStarts = 0;
    uint32_t workOverruns = 0;
    Phase phase = Phase::Idle;

    bool begin(uint64_t nowUs, const TaskTimingContract& contract);
    void end(uint64_t nowUs, uint32_t waitUs, const TaskTimingContract& contract);
    bool overdue(uint64_t nowUs, const TaskTimingContract& contract) const;
};

struct TaskRuntimeCounter {
    uint32_t counterUs = 0;
    uint32_t generation = 0;
    uint16_t taskId = 0;
};

struct TaskRuntimeSnapshot {
    static constexpr uint8_t kCapacity = 64;
    TaskRuntimeCounter tasks[kCapacity]{};
    uint64_t startedUs = 0;
    uint64_t endedUs = 0;
    uint8_t count = 0;
    bool available = false;
};

struct TaskRuntimeReader {
    void (*read)(void*, TaskRuntimeSnapshot&) = nullptr;
    void* context = nullptr;
};

/** Scratch storage used only by a task whose waits have CPU diagnostics enabled. */
struct TaskWakeRuntimeTrace {
    TaskRuntimeReader reader{};
    TaskRuntimeSnapshot before{};
    TaskRuntimeSnapshot after{};
};

enum class TaskWaitStage : uint8_t { Module, Loop };

struct TaskCpuContribution {
    uint32_t durationUs = 0;
    uint16_t taskId = 0;
};

struct TaskWaitSample {
    uint64_t startedUs = 0;
    uint64_t endedUs = 0;
    TickType_t startTick = 0;
    TickType_t endTick = 0;
    TickType_t requestedTicks = 0;
    uint32_t runtimeWindowUs = 0;
    uint32_t probeUs = 0;
    uint32_t accountedCpuUs = 0;
    uint8_t matchedTasks = 0;
    TaskCpuContribution top[3]{};

    TickType_t elapsedTicks() const { return endTick - startTick; }
    void recordRuntime(const TaskRuntimeSnapshot& before, const TaskRuntimeSnapshot& after);
};

struct TaskWakeSample {
    uint64_t expectedUs = 0;
    uint64_t actualUs = 0;
    uint64_t moduleDelayReturnedUs = 0;
    uint32_t waitUs = 0;
    uint32_t moduleWaitUs = 0;
    TaskWaitSample moduleWait{};
    TaskWaitSample loopWait{};

    uint64_t lateUs() const { return actualUs - expectedUs; }
};

struct TaskWakeInterval {
    uint64_t startUs = 0;
    uint64_t endUs = 0;

    bool overlaps(const TaskWakeSample& sample) const {
        return startUs < endUs && startUs < sample.actualUs && sample.expectedUs < endUs;
    }
};

struct TaskWakeBatch {
    uint8_t count = 0;
    uint32_t dropped = 0;
    TaskWakeSample worst{};
};

/** Optional PSRAM storage. Queue operations are serialized by the owning TaskHealth. */
class TaskWakeTrace {
public:
    static constexpr uint8_t kCapacity = 32;

private:
    friend class TaskHealth;
#if FLOW_PERFORMANCE_DIAGNOSTICS
    TaskWakeSample queue_[kCapacity]{};
    uint64_t moduleDelayReturnedUs_ = 0;
    uint32_t moduleWaitUs_ = 0;
    uint32_t dropped_ = 0;
    uint8_t nextRead_ = 0;
    uint8_t count_ = 0;
    TaskWakeSample worst_{};
    TaskWaitSample moduleWait_{};
    TaskWaitSample loopWait_{};
    TaskWakeRuntimeTrace* runtime_ = nullptr;

    void push(const TaskWakeSample& sample);
    bool pop(TaskWakeSample& sample);
#endif
};

struct TaskHealthSnapshot {
    TaskHandle_t handle = nullptr;
    uint64_t startedUs = 0;
    uint32_t generation = 0;
    uint32_t stackSamples = 0;
    uint32_t minFreeBytes = UINT32_MAX;
    TaskHealthState state = TaskHealthState::Expected;
    TaskTiming timing{};
};

struct TaskStackSample {
    uint64_t sampledAtUs = 0;
    uint64_t lastDurationUs = 0;
    uint64_t maxDurationUs = 0;
    uint32_t freeBytes = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t samples = 0;
};

enum class TaskStackReadStatus : uint8_t { Unavailable, Busy, Acquired };

/** The owner keeps a library task alive between acquire() and release(). */
struct TaskStackSource {
    const char* name = nullptr;
    uint32_t stackBytes = 0;
    uint16_t minFreeBytes = kDefaultTaskMinFreeBytes;
    TaskStackReadStatus (*acquire)(void*, TaskHandle_t&, uint32_t&) = nullptr;
    void (*release)(void*) = nullptr;
    void* context = nullptr;
    int8_t coreId = -1;
};

/** One stable register slot. The task owns timing updates; readers copy a snapshot. */
class TaskHealth {
public:
    /** Set once before creating the task. */
    void configure(TaskTimingContract contract, TaskHealthState state = TaskHealthState::Expected,
                   TaskWakeTrace* wakeTrace = nullptr, TaskWakeRuntimeTrace* runtime = nullptr) {
        contract_ = contract;
        stats_.state = state;
#if FLOW_PERFORMANCE_DIAGNOSTICS
        wakeTrace_ = wakeTrace;
        if (wakeTrace_) wakeTrace_->runtime_ = runtime;
#else
        (void)wakeTrace;
        (void)runtime;
#endif
    }
    TaskCadence cadence() const { return contract_.cadence; }
    TaskHealthSnapshot snapshot();
    bool requestStart();
    void started();
    void createFailed();
    void finish();
    void reaped();
    TaskHealthSnapshot recordStack(TaskHandle_t handle, uint32_t generation, uint32_t freeBytes);
    bool acquireStackRead(TaskHealthSnapshot& snapshot);
    void releaseStackRead();
    bool canReap();
#if FLOW_PERFORMANCE_DIAGNOSTICS
    void beginCycle();
    void endCycle(TickType_t waitTicks = 0, TickType_t moduleWaitTicks = 0);
    void recordModuleDelayReturn();
    void recordDelayStart(TaskWaitStage stage, TickType_t ticks);
    void recordDelayReturn(TaskWaitStage stage);
    TaskWakeBatch pendingWakeSamples();
    bool takeWakeSample(TaskWakeSample& sample);
#else
    void beginCycle() {}
    void endCycle(TickType_t = 0, TickType_t = 0) {}
    void recordModuleDelayReturn() {}
    void recordDelayStart(TaskWaitStage, TickType_t) {}
    void recordDelayReturn(TaskWaitStage) {}
    TaskWakeBatch pendingWakeSamples() { return {}; }
    bool takeWakeSample(TaskWakeSample&) { return false; }
#endif

private:
    TaskTimingContract contract_{};
#if FLOW_PERFORMANCE_DIAGNOSTICS
    TaskWakeTrace* wakeTrace_ = nullptr;
#endif
    TaskHealthSnapshot stats_{};
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    bool stackReadActive_ = false;
};

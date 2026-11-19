#pragma once

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Core/ModuleId.h"

enum class TaskRole : uint8_t { Primary = 0, Rs485 = 1, Reboot = 2, ArduinoLoop = 3 };
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

struct TaskWakeSample {
    uint64_t expectedUs = 0;
    uint64_t actualUs = 0;
    uint64_t moduleDelayReturnedUs = 0;
    uint32_t waitUs = 0;
    uint32_t moduleWaitUs = 0;

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
};

/** Optional PSRAM storage. Queue operations are serialized by the owning TaskHealth. */
class TaskWakeTrace {
public:
    static constexpr uint8_t kCapacity = 32;

private:
    friend class TaskHealth;
    TaskWakeSample queue_[kCapacity]{};
    uint64_t moduleDelayReturnedUs_ = 0;
    uint32_t moduleWaitUs_ = 0;
    uint32_t dropped_ = 0;
    uint8_t nextRead_ = 0;
    uint8_t count_ = 0;

    void push(const TaskWakeSample& sample);
    bool pop(TaskWakeSample& sample);
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

/** One stable register slot. The task owns timing updates; readers copy a snapshot. */
class TaskHealth {
public:
    /** Set once before creating the task. */
    void configure(TaskTimingContract contract, TaskHealthState state = TaskHealthState::Expected,
                   TaskWakeTrace* wakeTrace = nullptr) {
        contract_ = contract;
        stats_.state = state;
        wakeTrace_ = wakeTrace;
    }
    TaskCadence cadence() const { return contract_.cadence; }
    TaskHealthSnapshot snapshot();
    bool requestStart();
    void started();
    void createFailed();
    void finish();
    void reaped();
    TaskHealthSnapshot recordStack(TaskHandle_t handle, uint32_t generation, uint32_t freeBytes);
    void beginCycle();
    void endCycle(TickType_t waitTicks = 0, TickType_t moduleWaitTicks = 0);
    void recordModuleDelayReturn();
    TaskWakeBatch pendingWakeSamples();
    bool takeWakeSample(TaskWakeSample& sample);

private:
    TaskTimingContract contract_{};
    TaskWakeTrace* wakeTrace_ = nullptr;
    TaskHealthSnapshot stats_{};
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

#pragma once

#include "Core/EventBus/EventBus.h"
#include "Core/TaskHealth.h"

struct SystemHealthSnapshot {
    struct StackMargin {
        const char* task = nullptr;
        uint32_t freeBytes = 0;
        uint16_t thresholdBytes = 0;
        uint64_t sampledAtUs = 0;
    };
    struct WorkDuration {
        const char* task = nullptr;
        uint64_t durationUs = 0;
    };
    struct Tasks {
        uint8_t running = 0;
        uint8_t belowStackThreshold = 0;
        uint64_t lateStarts = 0;
        uint64_t workOverruns = 0;
        StackMargin lowestMargin{};
        WorkDuration longestWork{};
    };

    uint64_t sampledAtUs = 0;
    Tasks tasks{};
    bool eventBusAvailable = false;
    EventBus::QueueSnapshot eventBus{};
};

struct SystemHealthService {
    SystemHealthSnapshot (*snapshot)(void* ctx);
    void* ctx;
    TaskStackSample (*taskStack)(void* ctx, uint16_t taskId) = nullptr;
    TaskRuntimeReader wakeRuntimeReader{};
};

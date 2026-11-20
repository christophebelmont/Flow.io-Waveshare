#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "Core/TaskHealth.h"
#include "Core/DurationSamples.h"
#include <esp_timer.h>
using UBaseType_t = unsigned;

namespace Limits::Core::Capacity {
constexpr unsigned MaxTaskHealthEntries = 2U;
}

class ModuleManager {
public:
    struct TaskEntry {
        struct { int coreId = 1; } spec;
        uint16_t id = 0;
        mutable TaskHealth health;
    };
    uint8_t getTaskEntryCount() const { return 2; }
    const TaskEntry& getTaskEntry(uint8_t i) const { return entries[i]; }
    TaskEntry entries[2];
};

constexpr unsigned MALLOC_CAP_SPIRAM = 1U, MALLOC_CAP_8BIT = 2U;
constexpr unsigned eDeleted = 4U, tskNO_AFFINITY = 0xFFFFFFFFU;
unsigned captureCalls = 0, allocatedBuffers = 0;
bool failAllocation = false;
struct TaskStatus_t {
    unsigned eCurrentState = 0, xCoreID = 0, xTaskNumber = 1;
    unsigned uxBasePriority = 1, usStackHighWaterMark = 512;
    uint32_t ulRunTimeCounter = 100;
};
constexpr int pdFALSE = 0, eReady = 1;
using configRUN_TIME_COUNTER_TYPE = uint32_t;
int idleTask;
TaskHandle_t xTaskGetIdleTaskHandleForCore(int core) {
    assert(core == 1);
    return &idleTask;
}
void vTaskGetInfo(TaskHandle_t, TaskStatus_t* status, int readStack, int state) {
    assert(readStack == pdFALSE && state == eReady);
    *status = {};
    status->xCoreID = 1;
    fakeEspTimerUs += 3;
}
void* heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (failAllocation) return nullptr;
    ++allocatedBuffers;
    return calloc(count, size);
}
void heap_caps_free(void* pointer) {
    --allocatedBuffers;
    free(pointer);
}
unsigned uxTaskGetSystemState(TaskStatus_t* tasks, unsigned, void*) {
    ++captureCalls;
    tasks[0] = {};
    fakeEspTimerUs += 4000;
    return 1;
}
void logMessage(const char*, ...) {}
#define LOGD(...) logMessage(__VA_ARGS__)
#define configUSE_TRACE_FACILITY 1
#define configTASKLIST_INCLUDE_COREID 1
#define configGENERATE_RUN_TIME_STATS 1
#define CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER 1
struct CommandRequest {};

class SystemMonitorModule {
public:
    struct StackSlot {
        TaskStackSource source{};
        TaskStackSample sample{};
        uint16_t id = 0;
    };
    static constexpr unsigned kStackSamplePeriodMs = 100;
    static constexpr unsigned kTaskStatusSnapshotCapacity = 4;
    StackSlot slots[3]{};
    StackSlot* stackSlots_ = slots;
    ModuleManager* moduleManager = nullptr;
    uint8_t externalStackCount_ = 1, nextStackSample_ = 0;
    uint32_t lastStackSampleMs_ = 0;
    portMUX_TYPE stackSamplesMux_ = portMUX_INITIALIZER_UNLOCKED;
    std::atomic<bool> captureRequested_{false};
    TaskWakeInterval lastTaskCapture_{};
    DurationSamples taskCaptureDurations_{};
    TaskStackSample readTaskStack_(uint16_t taskId);
    void sampleTaskStack_(uint32_t nowMs);
    void captureTasksIfRequested_();
    void readCore1Runtime_(TaskRuntimeSnapshot&);
    static bool requestTaskCapture_(void*, const CommandRequest&, char*, size_t);
};

// PRODUCTION_METHODS

struct ExternalSource {
    TaskStackReadStatus status = TaskStackReadStatus::Acquired;
    unsigned generation = 1, reads = 0;
    bool held = false;
    static TaskStackReadStatus acquire(void* ctx, TaskHandle_t& handle, uint32_t& generation) {
        auto& self = *static_cast<ExternalSource*>(ctx);
        if (self.status != TaskStackReadStatus::Acquired) return self.status;
        assert(!self.held);
        self.held = true;
        handle = &self;
        generation = self.generation;
        ++self.reads;
        return self.status;
    }
    static void release(void* ctx) {
        auto& self = *static_cast<ExternalSource*>(ctx);
        assert(self.held);
        self.held = false;
    }
};
ExternalSource external;
unsigned stackReads = 0;
unsigned readStack(TaskHandle_t handle) {
    assert(handle);
    if (handle == &external) assert(external.held);
    ++stackReads;
    fakeEspTimerUs += 100;
    return fakeStackFree;
}

int main() {
    ModuleManager manager;
    manager.entries[0].id = 5632;
    manager.entries[0].health.configure({});
    assert(manager.entries[0].health.requestStart());
    int task = 0;
    fakeCurrentTask = &task;
    manager.entries[0].health.started();
    manager.entries[1].id = 5633; // Declared, still waiting to start.
    SystemMonitorModule monitor;
    monitor.moduleManager = &manager;
    monitor.slots[2].source = {"library", 2048, 512, ExternalSource::acquire, ExternalSource::release, &external, 1};
    monitor.slots[2].id = 5380;
    fakeStackRead = readStack;
    fakeStackFree = 1000;
    uint32_t now = 0;
    auto tick = [&](uint32_t ms) {
        now = ms;
        fakeEspTimerUs = uint64_t(ms) * 1000U;
        monitor.sampleTaskStack_(ms);
        monitor.captureTasksIfRequested_();
    };
    tick(99);
    assert(stackReads == 0);
    tick(100);
    assert(stackReads == 1);
    assert(monitor.readTaskStack_(5632).freeBytes == 1000);
    assert(monitor.readTaskStack_(5632).lastDurationUs == 100);
    tick(200);
    assert(stackReads == 1);
    tick(300);
    assert(stackReads == 2 && external.reads == 1 && !external.held);
    assert(monitor.readTaskStack_(5380).generation == 1);
    fakeStackFree = 700;
    tick(400);
    assert(monitor.readTaskStack_(5632).freeBytes == 700);
    tick(500);
    external.status = TaskStackReadStatus::Busy;
    tick(600);
    assert(monitor.readTaskStack_(5380).samples == 1);
    tick(700);
    tick(800);
    external.status = TaskStackReadStatus::Unavailable;
    tick(900);
    assert(monitor.readTaskStack_(5380).samples == 0);
    tick(1000);
    tick(1100);
    external.status = TaskStackReadStatus::Acquired;
    ++external.generation;
    fakeStackFree = 1500;
    tick(1200);
    assert(monitor.readTaskStack_(5380).freeBytes == 1500);
    assert(monitor.readTaskStack_(5380).generation == 2);
    const unsigned before = stackReads;
    tick(now + 10000);
    assert(stackReads == before + 1); // No catch-up burst.
    assert(captureCalls == 0);
    TaskRuntimeSnapshot runtime;
    monitor.readCore1Runtime_(runtime);
    assert(runtime.available && runtime.count == 2);
    assert(runtime.tasks[0].taskId == 5380 && runtime.tasks[0].generation == 2);
    assert(runtime.tasks[1].taskId == 5 && runtime.tasks[1].generation == 1);
    assert(!external.held && stackReads == before + 1 && captureCalls == 0);
    external.status = TaskStackReadStatus::Busy;
    monitor.readCore1Runtime_(runtime);
    assert(runtime.available && runtime.count == 1 && runtime.tasks[0].taskId == 5);
    char reply[64]{};
    assert(monitor.requestTaskCapture_(&monitor, {}, reply, sizeof(reply)));
    assert(monitor.requestTaskCapture_(&monitor, {}, reply, sizeof(reply)));
    assert(captureCalls == 0);
    monitor.captureTasksIfRequested_();
    assert(captureCalls == 1 && allocatedBuffers == 0);
    monitor.captureTasksIfRequested_();
    assert(captureCalls == 1);
    failAllocation = true;
    assert(monitor.requestTaskCapture_(&monitor, {}, reply, sizeof(reply)));
    monitor.captureTasksIfRequested_();
    assert(captureCalls == 1 && allocatedBuffers == 0 && !monitor.captureRequested_);
}

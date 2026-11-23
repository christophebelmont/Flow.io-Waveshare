#include "Core/TaskHealth.h"
#include "Core/DurationSamples.h"

#include <cassert>
#include <esp_timer.h>

#if FLOW_PERFORMANCE_DIAGNOSTICS
static uint32_t runtimeCounter = 0;

static void readRuntime(void*, TaskRuntimeSnapshot& out)
{
    out.startedUs = fakeEspTimerUs;
    out.tasks[0] = {runtimeCounter, 1U, 4100U};
    out.count = 1U;
    out.available = true;
    fakeEspTimerUs += 25;
    out.endedUs = fakeEspTimerUs;
}

static void testDetailedWaits()
{
    TaskWakeTrace trace;
    TaskWakeRuntimeTrace runtime;
    runtime.reader = {readRuntime, nullptr};
    TaskHealth health;
    health.configure({TaskCadence::Periodic, 25000, 2000}, TaskHealthState::Expected, &trace, &runtime);
    assert(health.requestStart());
    fakeEspTimerUs = 1000000U;
    health.started();
    health.beginCycle();
    fakeEspTimerUs += 100;
    health.endCycle(20, 10);
    fakeTaskTick = UINT32_MAX - 5U;
    health.recordDelayStart(TaskWaitStage::Module, 10);
    fakeEspTimerUs += 13000;
    fakeTaskTick += 13;
    runtimeCounter += 12000;
    health.recordModuleDelayReturn();
    health.recordDelayStart(TaskWaitStage::Loop, 10);
    fakeEspTimerUs += 12000;
    fakeTaskTick += 12;
    runtimeCounter += 11000;
    health.recordDelayReturn(TaskWaitStage::Loop);
    health.beginCycle();
    const auto batch = health.pendingWakeSamples();
    assert(batch.count == 1 && batch.worst.lateUs() == 5100U);
    TaskWakeSample sample;
    assert(health.takeWakeSample(sample));
    assert(sample.moduleWait.elapsedTicks() == 13 && sample.loopWait.elapsedTicks() == 12);
    assert(sample.moduleWait.requestedTicks == 10 && sample.loopWait.requestedTicks == 10);
    assert(sample.moduleWait.endedUs - sample.moduleWait.startedUs == 13000U);
    assert(sample.loopWait.endedUs - sample.loopWait.startedUs == 12000U);
    assert(sample.moduleWait.probeUs == 50 && sample.loopWait.probeUs == 50);
    assert(sample.moduleWait.runtimeWindowUs == 13050 && sample.loopWait.runtimeWindowUs == 12050);
    assert(sample.moduleWait.top[0].taskId == 4100 && sample.moduleWait.top[0].durationUs == 12000);
    assert(sample.loopWait.top[0].durationUs == 11000 && sample.loopWait.matchedTasks == 1);
    assert(sample.actualUs - sample.loopWait.endedUs == 25U);
}

static void testRuntimeDifferences()
{
    TaskRuntimeSnapshot before, after;
    before.available = after.available = true;
    before.startedUs = 100;
    before.endedUs = 150;
    after.startedUs = 2100;
    after.endedUs = 2200;
    before.count = after.count = 4;
    before.tasks[0] = {UINT32_MAX - 100U, 1U, 4100U};
    before.tasks[1] = {1000U, 1U, 512U};
    before.tasks[2] = {1000U, 1U, 5633U};
    before.tasks[3] = {1000U, 1U, 5U};
    after.tasks[0] = {1900U, 1U, 5U};
    after.tasks[1] = {100U, 1U, 4100U}; // CPU counter rollover.
    after.tasks[2] = {1800U, 1U, 512U};
    after.tasks[3] = {9000U, 2U, 5633U}; // Same identifier, a recreated task.
    TaskWaitSample wait;
    wait.recordRuntime(before, after);
    assert(wait.matchedTasks == 3 && wait.accountedCpuUs == 1901U);
    assert(wait.top[0].taskId == 5 && wait.top[0].durationUs == 900U);
    assert(wait.top[1].taskId == 512 && wait.top[2].taskId == 4100);
    assert(wait.probeUs == 150 && wait.runtimeWindowUs == 2100);
    after.available = false;
    TaskWaitSample unavailable;
    unavailable.recordRuntime(before, after);
    assert(unavailable.matchedTasks == 0);
}

static void testWakeTrace(uint64_t start, const TaskTimingContract& contract)
{
    TaskWakeTrace trace;
    TaskHealth health;
    health.configure(contract, TaskHealthState::Expected, &trace);
    assert(health.requestStart());
    fakeEspTimerUs = start;
    health.started();
    health.beginCycle();

    fakeEspTimerUs += 300;
    const uint64_t waitStart = fakeEspTimerUs;
    health.endCycle(20, 10);
    fakeEspTimerUs += 13000;
    health.recordModuleDelayReturn();
    fakeEspTimerUs += 12000;
    health.beginCycle();

    const auto first = health.pendingWakeSamples();
    assert(first.count == 1 && first.dropped == 0);
    TaskWakeSample sample;
    assert(health.takeWakeSample(sample) && !health.takeWakeSample(sample));
    assert(sample.expectedUs == waitStart + 20000U);
    assert(sample.actualUs == waitStart + 25000U && sample.lateUs() == 5000U);
    assert(sample.moduleDelayReturnedUs == waitStart + 13000U);
    assert(sample.waitUs == 20000U && sample.moduleWaitUs == 10000U);
    const TaskWakeInterval before{sample.expectedUs - 100U, sample.expectedUs};
    const TaskWakeInterval during{sample.expectedUs - 100U, sample.expectedUs + 1U};
    const TaskWakeInterval after{sample.actualUs, sample.actualUs + 100U};
    assert(!before.overlaps(sample) && during.overlaps(sample) && !after.overlaps(sample));
    assert(!TaskWakeInterval{}.overlaps(sample));

    health.endCycle(1);
    fakeEspTimerUs += 3000;
    health.beginCycle();
    assert(health.pendingWakeSamples().count == 0U); // Exactly at tolerance.

    const uint64_t burstStart = fakeEspTimerUs;
    for (unsigned i = 0; i < TaskWakeTrace::kCapacity + 3U; ++i) {
        health.endCycle(1);
        fakeEspTimerUs += 3001;
        health.beginCycle();
    }
    const auto burst = health.pendingWakeSamples();
    assert(burst.count == TaskWakeTrace::kCapacity && burst.dropped == 3U);
    assert(health.pendingWakeSamples().dropped == 0U);
    for (unsigned i = 0; i < burst.count; ++i) {
        assert(health.takeWakeSample(sample));
        assert(sample.actualUs == burstStart + (i + 1U) * 3001U);
        assert(sample.lateUs() == 2001U && sample.moduleDelayReturnedUs == 0U);
    }
    assert(!health.takeWakeSample(sample));
    assert(health.snapshot().timing.lateStarts == TaskWakeTrace::kCapacity + 4U);
    // Preserve the worst event even when it arrives after the queue fills.
    for (unsigned i = 0; i < TaskWakeTrace::kCapacity; ++i) {
        health.endCycle(1);
        fakeEspTimerUs += 3001;
        health.beginCycle();
    }
    health.endCycle(1);
    fakeEspTimerUs += 100000;
    health.beginCycle();
    const auto overflow = health.pendingWakeSamples();
    assert(overflow.count == TaskWakeTrace::kCapacity && overflow.dropped == 1);
    assert(overflow.worst.lateUs() == 99000U);
}

#else
static void testDisabledMeasurements()
{
    TaskWakeTrace trace;
    TaskWakeRuntimeTrace runtime;
    unsigned reads = 0;
    runtime.reader = {[](void* context, TaskRuntimeSnapshot&) {
        ++*static_cast<unsigned*>(context);
    }, &reads};
    TaskHealth health;
    health.configure({TaskCadence::Periodic, 1000, 2000}, TaskHealthState::Expected, &trace, &runtime);
    assert(health.requestStart());
    health.started();
    for (unsigned i = 0; i < 100; ++i) {
        health.beginCycle();
        health.endCycle(1, 1);
        health.recordDelayStart(TaskWaitStage::Module, 1);
        fakeEspTimerUs += 100000;
        health.recordModuleDelayReturn();
        health.recordDelayStart(TaskWaitStage::Loop, 1);
        health.recordDelayReturn(TaskWaitStage::Loop);
    }
    TaskWakeSample sample;
    assert(reads == 0 && health.pendingWakeSamples().count == 0);
    assert(!health.takeWakeSample(sample));
    const auto timing = health.snapshot().timing;
    assert(timing.cycles == 0 && timing.lateStarts == 0 && timing.workOverruns == 0);
    assert(timing.maxWorkUs == 0 && timing.maxCycleUs == 0);
}
#endif

int main()
{
    constexpr TaskTimingContract periodic{TaskCadence::Periodic, 1000, 2000};
    TaskTiming timing;
    constexpr uint64_t start = UINT32_MAX - 500U;
    timing.begin(start, periodic);
    assert(!timing.overdue(start + 1000U, periodic));
    assert(timing.overdue(start + 1001U, periodic));
    timing.end(start + 1200U, 10000U, periodic);
    assert(timing.cycles == 1 && timing.workOverruns == 1 && timing.maxWorkUs == 1200);
    assert(timing.expectedWaitUs == 10000);
    assert(!timing.overdue(start + 13200U, periodic));
    timing.begin(start + 13201U, periodic);
    assert(timing.lateStarts == 1 && timing.maxCycleUs == 13201);
    timing.end(start + 13501U, 20000U, periodic);
    assert(!timing.overdue(start + 35501U, periodic));
    assert(timing.overdue(start + 35502U, periodic));
    assert(timing.cycles == 2 && timing.workOverruns == 1 && timing.maxWorkUs == 1200);

    constexpr TaskTimingContract eventDriven{TaskCadence::EventDriven, 1000};
    TaskTiming event;
    event.begin(start, eventDriven);
    event.end(start + 10U, 0U, eventDriven);
    const uint64_t later = start + 2 * uint64_t{UINT32_MAX};
    assert(!event.overdue(later, eventDriven));
    event.begin(later, eventDriven);
    event.end(later + 50U, 0U, eventDriven);
    assert(event.lateStarts == 0 && event.maxCycleUs == 0 && event.maxWorkUs == 50);
    constexpr uint64_t longWorkUs = uint64_t{UINT32_MAX} + 10;
    event.begin(later + 100U, eventDriven);
    event.end(later + 100U + longWorkUs, 0U, eventDriven);
    assert(event.maxWorkUs == longWorkUs);

    DurationSamples durations;
    durations.record(longWorkUs);
    durations.record(250);
    assert(durations.count == 2 && durations.lastUs == 250 && durations.maxUs == longWorkUs);
    durations.count = UINT32_MAX;
    durations.record(0);
    assert(durations.count == UINT32_MAX && durations.lastUs == 0 && durations.maxUs == longWorkUs);

    int task = 0;
    fakeCurrentTask = &task;
#if FLOW_PERFORMANCE_DIAGNOSTICS
    testDetailedWaits();
    testRuntimeDifferences();
    testWakeTrace(later, periodic);
#else
    testDisabledMeasurements();
#endif
    TaskHealth health;
    health.configure(periodic);
    TaskHealthSnapshot readSnapshot{};
    assert(!health.acquireStackRead(readSnapshot) && !health.canReap());
    assert(health.requestStart() && !health.requestStart());
    fakeEspTimerUs = start;
    health.started();
    auto snapshot = health.snapshot();
    assert(snapshot.state == TaskHealthState::Running && snapshot.generation == 1);
    assert(snapshot.handle == fakeCurrentTask && snapshot.startedUs == start);
    assert(health.acquireStackRead(readSnapshot));
    assert(readSnapshot.handle == fakeCurrentTask && readSnapshot.generation == 1U);
    assert(!health.acquireStackRead(readSnapshot));
    health.recordStack(fakeCurrentTask, 1, 800);
    fakeStackFree = 700;
    health.beginCycle();
    fakeEspTimerUs += 300;
    health.endCycle(5);
    health.finish();
    snapshot = health.snapshot();
    assert(snapshot.state == TaskHealthState::Finishing);
    assert(!health.canReap() && !health.acquireStackRead(readSnapshot));
    health.releaseStackRead();
    assert(health.canReap());
    assert(snapshot.stackSamples == 2 && snapshot.minFreeBytes == 700);
#if FLOW_PERFORMANCE_DIAGNOSTICS
    assert(snapshot.timing.cycles == 1 && snapshot.timing.expectedWaitUs == 5000);
#else
    assert(snapshot.timing.cycles == 0 && snapshot.timing.expectedWaitUs == 0);
#endif
    health.recordStack(fakeCurrentTask, 1, 0);
    health.reaped();
    snapshot = health.snapshot();
    assert(snapshot.state == TaskHealthState::Finished && !snapshot.handle && snapshot.minFreeBytes == 700);
    assert(health.requestStart());
    health.createFailed();
    assert(health.snapshot().state == TaskHealthState::CreateFailed);
    assert(health.requestStart());
    health.started();
    health.recordStack(fakeCurrentTask, 1, 0);
    health.recordStack(fakeCurrentTask, 3, 650);
    snapshot = health.snapshot();
    assert(snapshot.stackSamples == 3 && snapshot.minFreeBytes == 650);
    assert(snapshot.timing.phase == TaskTiming::Phase::Idle && snapshot.timing.expectedWaitUs == 0U);

    TaskHealth disabled;
    disabled.configure(eventDriven, TaskHealthState::Disabled);
    assert(!disabled.requestStart() && disabled.snapshot().state == TaskHealthState::Disabled);
    assert(taskHealthId(ModuleId::Io, TaskRole::Primary) != taskHealthId(ModuleId::Io, TaskRole::Rs485));
}

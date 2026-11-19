#include "Core/TaskHealth.h"
#include "Core/DurationSamples.h"

#include <cassert>
#include <esp_timer.h>

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
}

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
    testWakeTrace(later, periodic);
    TaskHealth health;
    health.configure(periodic);
    assert(health.requestStart() && !health.requestStart());
    fakeEspTimerUs = start;
    health.started();
    auto snapshot = health.snapshot();
    assert(snapshot.state == TaskHealthState::Running && snapshot.generation == 1);
    assert(snapshot.handle == fakeCurrentTask && snapshot.startedUs == start);
    health.recordStack(fakeCurrentTask, 1, 800);
    fakeStackFree = 700;
    health.beginCycle();
    fakeEspTimerUs += 300;
    health.endCycle(5);
    health.finish();
    snapshot = health.snapshot();
    assert(snapshot.state == TaskHealthState::Finishing);
    assert(snapshot.stackSamples == 2 && snapshot.minFreeBytes == 700);
    assert(snapshot.timing.cycles == 1 && snapshot.timing.expectedWaitUs == 5000);
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

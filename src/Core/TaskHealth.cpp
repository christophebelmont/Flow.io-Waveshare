#include "Core/TaskHealth.h"

#include <algorithm>
#include <esp_timer.h>

#include "Core/SaturatingCounter.h"

namespace {
uint32_t waitTicksToUs(TickType_t ticks)
{
    const uint64_t waitUs = uint64_t(ticks) * 1000000U / configTICK_RATE_HZ;
    return waitUs > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(waitUs);
}
}

bool TaskTiming::begin(uint64_t nowUs, const TaskTimingContract& contract)
{
    bool late = false;
    if (contract.cadence == TaskCadence::Periodic && phase == Phase::Waiting) {
        maxCycleUs = std::max(maxCycleUs, nowUs - workStartedUs);
        late = overdue(nowUs, contract);
        if (late) incrementSaturated(lateStarts);
    }
    workStartedUs = nowUs;
    phase = Phase::Working;
    return late;
}

void TaskTiming::end(uint64_t nowUs, uint32_t waitUs, const TaskTimingContract& contract)
{
    if (phase != Phase::Working) return;
    const uint64_t workUs = nowUs - workStartedUs;
    maxWorkUs = std::max(maxWorkUs, workUs);
    if (contract.workBudgetUs != 0U && workUs > contract.workBudgetUs) {
        incrementSaturated(workOverruns);
    }
    incrementSaturated(cycles);
    phase = Phase::Waiting;
    lastProgressUs = nowUs;
    expectedWaitUs = waitUs;
}

bool TaskTiming::overdue(uint64_t nowUs, const TaskTimingContract& contract) const
{
    if (phase == Phase::Working) {
        return contract.workBudgetUs != 0U && nowUs > workStartedUs + contract.workBudgetUs;
    }
    return phase == Phase::Waiting && contract.cadence == TaskCadence::Periodic &&
           nowUs > lastProgressUs + expectedWaitUs + contract.wakeToleranceUs;
}

void TaskWakeTrace::push(const TaskWakeSample& sample)
{
    if (count_ == kCapacity) {
        incrementSaturated(dropped_);
        return;
    }
    queue_[(nextRead_ + count_) % kCapacity] = sample;
    ++count_;
}

bool TaskWakeTrace::pop(TaskWakeSample& sample)
{
    if (count_ == 0U) return false;
    sample = queue_[nextRead_];
    nextRead_ = (nextRead_ + 1U) % kCapacity;
    --count_;
    return true;
}

TaskHealthSnapshot TaskHealth::snapshot()
{
    portENTER_CRITICAL(&mux_);
    const TaskHealthSnapshot snapshot = stats_;
    portEXIT_CRITICAL(&mux_);
    return snapshot;
}

bool TaskHealth::requestStart()
{
    portENTER_CRITICAL(&mux_);
    const bool available = stats_.state == TaskHealthState::Expected ||
                           stats_.state == TaskHealthState::Finished ||
                           stats_.state == TaskHealthState::CreateFailed;
    if (available) {
        incrementSaturated(stats_.generation);
        stats_.state = TaskHealthState::Starting;
        stats_.timing.phase = TaskTiming::Phase::Idle;
        stats_.timing.expectedWaitUs = 0;
        stats_.timing.lastProgressUs = 0;
    }
    portEXIT_CRITICAL(&mux_);
    return available;
}

void TaskHealth::started()
{
    const auto handle = xTaskGetCurrentTaskHandle();
    const uint64_t nowUs = esp_timer_get_time();
    portENTER_CRITICAL(&mux_);
    stats_.handle = handle;
    stats_.startedUs = nowUs;
    stats_.state = TaskHealthState::Running;
    portEXIT_CRITICAL(&mux_);
}

void TaskHealth::createFailed()
{
    portENTER_CRITICAL(&mux_);
    stats_.state = TaskHealthState::CreateFailed;
    portEXIT_CRITICAL(&mux_);
}

TaskHealthSnapshot TaskHealth::recordStack(TaskHandle_t handle, uint32_t generation, uint32_t freeBytes)
{
    portENTER_CRITICAL(&mux_);
    if (stats_.state == TaskHealthState::Running && stats_.handle == handle &&
        stats_.generation == generation) {
        if (freeBytes < stats_.minFreeBytes) stats_.minFreeBytes = freeBytes;
        incrementSaturated(stats_.stackSamples);
    }
    const auto snapshot = stats_;
    portEXIT_CRITICAL(&mux_);
    return snapshot;
}

void TaskHealth::beginCycle()
{
    const uint64_t nowUs = esp_timer_get_time();
    portENTER_CRITICAL(&mux_);
    auto& timing = stats_.timing;
    if (timing.begin(nowUs, contract_) && wakeTrace_) {
        wakeTrace_->push({
            timing.lastProgressUs + timing.expectedWaitUs,
            nowUs,
            wakeTrace_->moduleDelayReturnedUs_,
            timing.expectedWaitUs,
            wakeTrace_->moduleWaitUs_,
        });
    }
    portEXIT_CRITICAL(&mux_);
}

void TaskHealth::endCycle(TickType_t waitTicks, TickType_t moduleWaitTicks)
{
    const uint64_t nowUs = esp_timer_get_time();
    const uint32_t waitUs = waitTicksToUs(waitTicks);
    if (wakeTrace_) {
        wakeTrace_->moduleDelayReturnedUs_ = 0U;
        wakeTrace_->moduleWaitUs_ = waitTicksToUs(moduleWaitTicks);
    }
    portENTER_CRITICAL(&mux_);
    stats_.timing.end(nowUs, waitUs, contract_);
    portEXIT_CRITICAL(&mux_);
}

void TaskHealth::recordModuleDelayReturn()
{
    if (wakeTrace_) wakeTrace_->moduleDelayReturnedUs_ = esp_timer_get_time();
}

TaskWakeBatch TaskHealth::pendingWakeSamples()
{
    if (!wakeTrace_) return {};
    portENTER_CRITICAL(&mux_);
    const TaskWakeBatch batch{wakeTrace_->count_, wakeTrace_->dropped_};
    wakeTrace_->dropped_ = 0U;
    portEXIT_CRITICAL(&mux_);
    return batch;
}

bool TaskHealth::takeWakeSample(TaskWakeSample& sample)
{
    if (!wakeTrace_) return false;
    portENTER_CRITICAL(&mux_);
    const bool taken = wakeTrace_->pop(sample);
    portEXIT_CRITICAL(&mux_);
    return taken;
}

void TaskHealth::finish()
{
    endCycle();
    const uint32_t freeBytes = uxTaskGetStackHighWaterMark(nullptr);
    portENTER_CRITICAL(&mux_);
    if (freeBytes < stats_.minFreeBytes) stats_.minFreeBytes = freeBytes;
    incrementSaturated(stats_.stackSamples);
    stats_.state = TaskHealthState::Finishing;
    portEXIT_CRITICAL(&mux_);
}

void TaskHealth::reaped()
{
    portENTER_CRITICAL(&mux_);
    stats_.handle = nullptr;
    stats_.state = TaskHealthState::Finished;
    portEXIT_CRITICAL(&mux_);
}

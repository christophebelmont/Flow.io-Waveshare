/**
 * @file PoolHistoryAccumulator.cpp
 * @brief Deterministic daily pool history aggregation.
 */

#include "Modules/PoolHistoryModule/PoolHistoryAccumulator.h"

#include <limits.h>
#include <math.h>

namespace {

uint32_t secondsFromMs_(uint64_t milliseconds)
{
    const uint64_t seconds = milliseconds / 1000ULL;
    return (seconds > UINT32_MAX) ? UINT32_MAX : (uint32_t)seconds;
}

const PoolHistoryDayState* newestForDate_(uint32_t date,
                                          const PoolHistoryDayState* first,
                                          const PoolHistoryDayState* second)
{
    const bool firstMatches = first && first->valid && first->localDate == date;
    const bool secondMatches = second && second->valid && second->localDate == date;
    if (!firstMatches) return secondMatches ? second : nullptr;
    if (!secondMatches) return first;
    return (second->observedUntilUtc > first->observedUntilUtc) ? second : first;
}

}  // namespace

void PoolHistoryAccumulator::reset()
{
    today_ = PoolHistoryDayState{};
    previousDay_ = PoolHistoryDayState{};
}

PoolHistoryDayState PoolHistoryAccumulator::makeDay_(uint32_t localDate,
                                                      uint64_t dayStartUtc)
{
    PoolHistoryDayState day{};
    day.valid = true;
    day.localDate = localDate;
    day.dayStartUtc = dayStartUtc;
    return day;
}

void PoolHistoryAccumulator::restoreForDate(uint32_t currentDate,
                                            uint32_t previousDate,
                                            uint64_t currentDayStartUtc,
                                            const PoolHistoryDayState* first,
                                            const PoolHistoryDayState* second)
{
    reset();

    if (const PoolHistoryDayState* current = newestForDate_(currentDate, first, second)) {
        today_ = *current;
        today_.complete = false;
        today_.dayStartUtc = currentDayStartUtc;
    } else {
        today_ = makeDay_(currentDate, currentDayStartUtc);
    }

    if (const PoolHistoryDayState* previous = newestForDate_(previousDate, first, second)) {
        previousDay_ = *previous;
        previousDay_.complete = true;
    }
}

PoolHistoryDayTransition PoolHistoryAccumulator::alignDay(uint32_t currentDate,
                                                          uint32_t previousDate,
                                                          uint64_t currentDayStartUtc)
{
    if (!today_.valid) {
        today_ = makeDay_(currentDate, currentDayStartUtc);
        return PoolHistoryDayTransition::Initialized;
    }
    if (today_.localDate == currentDate) return PoolHistoryDayTransition::None;

    const bool isImmediateSuccessor = today_.localDate == previousDate;
    if (isImmediateSuccessor) {
        previousDay_ = today_;
        previousDay_.complete = true;
    } else {
        previousDay_ = PoolHistoryDayState{};
    }
    today_ = makeDay_(currentDate, currentDayStartUtc);
    return isImmediateSuccessor
        ? PoolHistoryDayTransition::AdvancedOneDay
        : PoolHistoryDayTransition::Realigned;
}

void PoolHistoryAccumulator::noteObservation_(PoolHistoryDayState& day,
                                              uint64_t observedAtUtc)
{
    if (observedAtUtc == 0U) return;
    if (day.observedFromUtc == 0U || observedAtUtc < day.observedFromUtc) {
        day.observedFromUtc = observedAtUtc;
    }
    if (observedAtUtc > day.observedUntilUtc) {
        day.observedUntilUtc = observedAtUtc;
    }
}

void PoolHistoryAccumulator::addSample(PoolHistoryMetric metric,
                                       float value,
                                       uint64_t observedAtUtc)
{
    const uint8_t metricIndex = (uint8_t)metric;
    if (!today_.valid || metricIndex >= (uint8_t)PoolHistoryMetric::Count || !isfinite(value)) {
        return;
    }

    PoolHistoryMetricState& state = today_.metrics[metricIndex];
    if (state.sampleCount == UINT32_MAX) return;
    if (state.sampleCount == 0U) {
        state.first = value;
        state.minimum = value;
        state.maximum = value;
    } else {
        if (value < state.minimum) state.minimum = value;
        if (value > state.maximum) state.maximum = value;
    }
    state.last = value;
    state.sum += (double)value;
    ++state.sampleCount;
    noteObservation_(today_, observedAtUtc);
}

void PoolHistoryAccumulator::observeFiltration(uint32_t intervalMs,
                                               bool running,
                                               uint64_t observedAtUtc)
{
    if (!today_.valid || intervalMs == 0U) return;
    today_.filtrationObservedMs += intervalMs;
    if (running) today_.filtrationRunningMs += intervalMs;
    noteObservation_(today_, observedAtUtc);
}

void PoolHistoryAccumulator::fillMetricSummary_(const PoolHistoryMetricState& state,
                                                PoolHistoryMetricSummary& out)
{
    out = PoolHistoryMetricSummary{};
    if (state.sampleCount == 0U) return;
    out.valid = true;
    out.sampleCount = state.sampleCount;
    out.first = state.first;
    out.last = state.last;
    out.minimum = state.minimum;
    out.maximum = state.maximum;
    out.average = (float)(state.sum / (double)state.sampleCount);
}

void PoolHistoryAccumulator::fillDaySummary_(const PoolHistoryDayState& state,
                                             PoolHistoryDaySummary& out)
{
    out = PoolHistoryDaySummary{};
    if (!state.valid) return;

    out.valid = true;
    out.complete = state.complete;
    out.localDate = state.localDate;
    out.dayStartUtc = state.dayStartUtc;
    out.observedFromUtc = state.observedFromUtc;
    out.observedUntilUtc = state.observedUntilUtc;
    out.filtrationRunningSec = secondsFromMs_(state.filtrationRunningMs);
    out.filtrationObservedSec = secondsFromMs_(state.filtrationObservedMs);
    fillMetricSummary_(state.metrics[(uint8_t)PoolHistoryMetric::Ph], out.ph);
    fillMetricSummary_(state.metrics[(uint8_t)PoolHistoryMetric::Orp], out.orp);
    fillMetricSummary_(state.metrics[(uint8_t)PoolHistoryMetric::WaterTemperature],
                       out.waterTemperature);
    fillMetricSummary_(state.metrics[(uint8_t)PoolHistoryMetric::AirTemperature],
                       out.airTemperature);
}

void PoolHistoryAccumulator::snapshot(uint64_t generatedAtUtc,
                                      PoolHistorySnapshot& out) const
{
    out = PoolHistorySnapshot{};
    out.generatedAtUtc = generatedAtUtc;
    fillDaySummary_(today_, out.today);
    fillDaySummary_(previousDay_, out.previousDay);
}

/**
 * @file PoolHistoryAccumulator.cpp
 * @brief Deterministic daily pool history aggregation.
 */

#include "Modules/PoolHistoryModule/PoolHistoryAccumulator.h"

#include <limits.h>
#include <math.h>
#include <string.h>

namespace {

uint32_t secondsFromMs_(uint64_t milliseconds)
{
    const uint64_t seconds = milliseconds / 1000ULL;
    return (seconds > UINT32_MAX) ? UINT32_MAX : (uint32_t)seconds;
}

const PoolHistoryDayState* newestForDate_(uint32_t date,
                                          const PoolHistoryDayState* records,
                                          uint8_t recordCount)
{
    const PoolHistoryDayState* newest = nullptr;
    if (!records) return nullptr;
    for (uint8_t i = 0U; i < recordCount; ++i) {
        const PoolHistoryDayState& candidate = records[i];
        if (!candidate.valid || candidate.localDate != date) continue;
        if (!newest || candidate.observedUntilUtc > newest->observedUntilUtc) {
            newest = &candidate;
        }
    }
    return newest;
}

}  // namespace

void PoolHistorySamplingGate::initialize(bool known, bool running)
{
    known_ = known;
    running_ = running;
    continuousRunningMs_ = 0U;
}

void PoolHistorySamplingGate::accrue(uint32_t intervalMs, bool intervalUsable)
{
    if (!intervalUsable || !known_ || !running_) {
        continuousRunningMs_ = 0U;
        return;
    }
    if (UINT64_MAX - continuousRunningMs_ < intervalMs) {
        continuousRunningMs_ = UINT64_MAX;
    } else {
        continuousRunningMs_ += intervalMs;
    }
}

void PoolHistorySamplingGate::update(bool known, bool running)
{
    if (!known || !running || !known_ || !running_) continuousRunningMs_ = 0U;
    known_ = known;
    running_ = running;
}

uint32_t PoolHistorySamplingGate::maximumEligibleSampleAgeMs(
    uint64_t minimumRunningMs,
    uint32_t absoluteMaximumAgeMs) const
{
    if (!eligible(minimumRunningMs)) return 0U;
    const uint64_t eligibleDurationMs = continuousRunningMs_ - minimumRunningMs;
    return eligibleDurationMs < absoluteMaximumAgeMs
        ? (uint32_t)eligibleDurationMs
        : absoluteMaximumAgeMs;
}

void PoolHistoryAccumulator::reset()
{
    today_ = PoolHistoryDayState{};
    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        completedDays_[i] = PoolHistoryDayState{};
    }
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
                                            uint64_t currentDayStartUtc,
                                            const uint32_t expectedCompleteDates[POOL_HISTORY_COMPLETE_DAY_COUNT],
                                            const PoolHistoryDayState* records,
                                            uint8_t recordCount)
{
    reset();

    if (const PoolHistoryDayState* current = newestForDate_(currentDate, records, recordCount)) {
        today_ = *current;
        today_.complete = false;
        today_.dayStartUtc = currentDayStartUtc;
    } else {
        today_ = makeDay_(currentDate, currentDayStartUtc);
    }

    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        if (const PoolHistoryDayState* completed =
                newestForDate_(expectedCompleteDates[i], records, recordCount)) {
            completedDays_[i] = *completed;
            completedDays_[i].complete = true;
        }
    }
}

const PoolHistoryDayState* PoolHistoryAccumulator::findCompletedDate_(uint32_t localDate) const
{
    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        if (completedDays_[i].valid && completedDays_[i].localDate == localDate) {
            return &completedDays_[i];
        }
    }
    return nullptr;
}

PoolHistoryDayTransition PoolHistoryAccumulator::alignDay(
    uint32_t currentDate,
    uint64_t currentDayStartUtc,
    const uint32_t expectedCompleteDates[POOL_HISTORY_COMPLETE_DAY_COUNT])
{
    if (!today_.valid) {
        today_ = makeDay_(currentDate, currentDayStartUtc);
        return PoolHistoryDayTransition::Initialized;
    }
    if (today_.localDate == currentDate) return PoolHistoryDayTransition::None;

    const bool isImmediateSuccessor = today_.localDate == expectedCompleteDates[0];
    const PoolHistoryDayState closedToday = today_;
    if (currentDate > closedToday.localDate) {
        // Moving backwards avoids a second seven-day array: on a forward date
        // transition every retained source is at a lower index than its target.
        for (int8_t i = (int8_t)POOL_HISTORY_COMPLETE_DAY_COUNT - 1; i >= 0; --i) {
            const uint32_t expectedDate = expectedCompleteDates[(uint8_t)i];
            PoolHistoryDayState selected{};
            if (closedToday.valid && closedToday.localDate == expectedDate) {
                selected = closedToday;
                selected.complete = true;
            } else if (const PoolHistoryDayState* existing = findCompletedDate_(expectedDate)) {
                selected = *existing;
                selected.complete = true;
            }
            completedDays_[(uint8_t)i] = selected;
        }
    } else {
        for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
            completedDays_[i] = PoolHistoryDayState{};
        }
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

    addMetricValue_(today_.metrics[metricIndex], value, observedAtUtc, today_);
}

void PoolHistoryAccumulator::addMetricValue_(PoolHistoryMetricState& state,
                                             float value,
                                             uint64_t observedAtUtc,
                                             PoolHistoryDayState& day)
{
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
    noteObservation_(day, observedAtUtc);
}

void PoolHistoryAccumulator::addWaterTemperatureSample(float value,
                                                        bool daytime,
                                                        uint64_t observedAtUtc)
{
    if (!today_.valid || !isfinite(value)) return;
    addMetricValue_(daytime ? today_.daytimeWaterTemperature
                            : today_.nighttimeWaterTemperature,
                    value,
                    observedAtUtc,
                    today_);
}

void PoolHistoryAccumulator::observeRefill(uint32_t intervalMs,
                                           bool running,
                                           float flowLPerHour,
                                           bool eventStarted,
                                           uint64_t observedAtUtc)
{
    if (!today_.valid) return;
    today_.refillStateObserved = true;
    if (eventStarted && today_.refillEventCount < UINT32_MAX) {
        ++today_.refillEventCount;
        noteObservation_(today_, observedAtUtc);
    }
    if (!running || intervalMs == 0U) return;
    if (!isfinite(flowLPerHour) || flowLPerHour <= 0.0f) {
        today_.refillVolumeValid = false;
        noteObservation_(today_, observedAtUtc);
        return;
    }
    today_.refillVolumeLitres +=
        (double)flowLPerHour * ((double)intervalMs / 3600000.0);
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
    out.filtrationRuntimeValid = state.filtrationObservedMs > 0U;
    out.filtrationRunningSec = secondsFromMs_(state.filtrationRunningMs);
    out.filtrationRuntimeMinutes = out.filtrationRunningSec / 60U;
    out.filtrationRuntimeHours = (float)((double)state.filtrationRunningMs / 3600000.0);
    out.filtrationObservedSec = secondsFromMs_(state.filtrationObservedMs);
    fillMetricSummary_(state.metrics[(uint8_t)PoolHistoryMetric::Ph], out.ph);
    fillMetricSummary_(state.metrics[(uint8_t)PoolHistoryMetric::Orp], out.orp);
    fillMetricSummary_(state.metrics[(uint8_t)PoolHistoryMetric::WaterTemperature],
                       out.waterTemperature);
    fillMetricSummary_(state.metrics[(uint8_t)PoolHistoryMetric::AirTemperature],
                       out.airTemperature);
    fillMetricSummary_(state.daytimeWaterTemperature, out.daytimeWaterTemperature);
    fillMetricSummary_(state.nighttimeWaterTemperature, out.nighttimeWaterTemperature);
    if (out.daytimeWaterTemperature.valid && out.nighttimeWaterTemperature.valid) {
        out.dayToNightTemperatureVariationValid = true;
        out.dayToNightTemperatureVariationC =
            out.nighttimeWaterTemperature.average - out.daytimeWaterTemperature.average;
    }
    out.refillVolumeValid = state.refillStateObserved && state.refillVolumeValid;
    if (out.refillVolumeValid) out.refillVolumeLitres = (float)state.refillVolumeLitres;
    out.refillEventsValid = state.refillStateObserved;
    out.refillEventCount = state.refillEventCount;
}

void PoolHistoryAccumulator::snapshot(
    uint64_t generatedAtUtc,
    const uint32_t expectedCompleteDates[POOL_HISTORY_COMPLETE_DAY_COUNT],
    const uint64_t expectedCompleteStartsUtc[POOL_HISTORY_COMPLETE_DAY_COUNT],
    uint8_t daytimeStartHour,
    uint8_t daytimeEndHour,
    const PoolCharacteristics& pool,
    PoolHistorySnapshot& out) const
{
    memset(&out, 0, sizeof(out));
    out.generatedAtUtc = generatedAtUtc;
    out.daytimeStartHour = daytimeStartHour;
    out.daytimeEndHour = daytimeEndHour;
    out.pool = pool;
    out.last7Days.requestedDayCount = POOL_HISTORY_COMPLETE_DAY_COUNT;
    fillDaySummary_(today_, out.today);
    uint64_t filtrationTotalMs = 0U;
    double refillTotalLitres = 0.0;
    bool allAvailableRefillVolumesValid = true;
    bool allAvailableRefillEventsValid = true;
    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        PoolHistoryDaySummary& day = out.completeDays[i];
        const PoolHistoryDayState* state = findCompletedDate_(expectedCompleteDates[i]);
        if (state) {
            fillDaySummary_(*state, day);
            ++out.last7Days.availableDayCount;
            if (day.filtrationRuntimeValid) {
                ++out.last7Days.filtrationAvailableDayCount;
                filtrationTotalMs += state->filtrationRunningMs;
            }
            out.last7Days.totalRefillEventCount += day.refillEventCount;
            if (!day.refillEventsValid) allAvailableRefillEventsValid = false;
            if (day.refillVolumeValid) refillTotalLitres += state->refillVolumeLitres;
            else allAvailableRefillVolumesValid = false;
        } else {
            day.complete = true;
            day.localDate = expectedCompleteDates[i];
            day.dayStartUtc = expectedCompleteStartsUtc[i];
        }
    }
    out.previousDay = out.completeDays[0];
    out.last7Days.totalFiltrationSec = filtrationTotalMs / 1000ULL;
    out.last7Days.totalFiltrationMinutes =
        (uint32_t)(out.last7Days.totalFiltrationSec / 60ULL);
    out.last7Days.totalFiltrationHours =
        (float)((double)filtrationTotalMs / 3600000.0);
    if (out.last7Days.filtrationAvailableDayCount > 0U) {
        out.last7Days.filtrationRuntimeValid = true;
        out.last7Days.averageDailyFiltrationValid = true;
        out.last7Days.averageDailyFiltrationHours =
            (float)(((double)filtrationTotalMs / 3600000.0) /
                    (double)out.last7Days.filtrationAvailableDayCount);
    }
    if (out.last7Days.availableDayCount > 0U) {
        out.last7Days.refillVolumeValid = allAvailableRefillVolumesValid;
        out.last7Days.refillEventsValid = allAvailableRefillEventsValid;
        if (allAvailableRefillVolumesValid) {
            out.last7Days.totalRefillVolumeLitres = (float)refillTotalLitres;
            out.last7Days.averageDailyRefillVolumeLitres =
                (float)(refillTotalLitres / (double)out.last7Days.availableDayCount);
        }
    }
}

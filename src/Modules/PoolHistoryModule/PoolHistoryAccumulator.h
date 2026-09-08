#pragma once
/**
 * @file PoolHistoryAccumulator.h
 * @brief Deterministic daily aggregation used by PoolHistoryModule.
 */

#include <stdint.h>

#include "Core/Services/IPoolHistory.h"

enum class PoolHistoryMetric : uint8_t {
    Ph = 0,
    Orp,
    WaterTemperature,
    AirTemperature,
    Count
};

struct PoolHistoryMetricState {
    uint32_t sampleCount = 0U;
    float first = 0.0f;
    float last = 0.0f;
    float minimum = 0.0f;
    float maximum = 0.0f;
    double sum = 0.0;
};

struct PoolHistoryDayState {
    bool valid = false;
    bool complete = false;
    uint32_t localDate = 0U;
    uint64_t dayStartUtc = 0U;
    uint64_t observedFromUtc = 0U;
    uint64_t observedUntilUtc = 0U;
    uint64_t filtrationRunningMs = 0U;
    uint64_t filtrationObservedMs = 0U;
    PoolHistoryMetricState metrics[(uint8_t)PoolHistoryMetric::Count]{};
    PoolHistoryMetricState daytimeWaterTemperature{};
    PoolHistoryMetricState nighttimeWaterTemperature{};
    bool refillVolumeValid = true;
    bool refillStateObserved = false;
    double refillVolumeLitres = 0.0;
    uint32_t refillEventCount = 0U;
};

enum class PoolHistoryDayTransition : uint8_t {
    None = 0,
    Initialized,
    AdvancedOneDay,
    Realigned
};

/** Tracks uninterrupted filtration time without depending on wall-clock arithmetic. */
class PoolHistorySamplingGate {
public:
    void initialize(bool known, bool running);
    void accrue(uint32_t intervalMs, bool intervalUsable);
    void update(bool known, bool running);
    bool eligible(uint64_t minimumRunningMs) const {
        return known_ && running_ && continuousRunningMs_ >= minimumRunningMs;
    }
    uint32_t maximumEligibleSampleAgeMs(uint64_t minimumRunningMs,
                                        uint32_t absoluteMaximumAgeMs) const;
    bool known() const { return known_; }
    bool running() const { return running_; }
    uint64_t continuousRunningMs() const { return continuousRunningMs_; }

private:
    bool known_ = false;
    bool running_ = false;
    uint64_t continuousRunningMs_ = 0U;
};

class PoolHistoryAccumulator {
public:
    void reset();

    /**
     * Restore persisted records matching the current day and the seven expected
     * complete local dates.
     */
    void restoreForDate(uint32_t currentDate,
                        uint64_t currentDayStartUtc,
                        const uint32_t expectedCompleteDates[POOL_HISTORY_COMPLETE_DAY_COUNT],
                        const PoolHistoryDayState* records,
                        uint8_t recordCount);

    /** Align the active record with the supplied local date. */
    PoolHistoryDayTransition alignDay(
        uint32_t currentDate,
        uint64_t currentDayStartUtc,
        const uint32_t expectedCompleteDates[POOL_HISTORY_COMPLETE_DAY_COUNT]);

    void addSample(PoolHistoryMetric metric, float value, uint64_t observedAtUtc);
    void addWaterTemperatureSample(float value, bool daytime, uint64_t observedAtUtc);
    void observeFiltration(uint32_t intervalMs, bool running, uint64_t observedAtUtc);
    void observeRefill(uint32_t intervalMs,
                       bool running,
                       float flowLPerHour,
                       bool eventStarted,
                       uint64_t observedAtUtc);
    void snapshot(
        uint64_t generatedAtUtc,
        const uint32_t expectedCompleteDates[POOL_HISTORY_COMPLETE_DAY_COUNT],
        const uint64_t expectedCompleteStartsUtc[POOL_HISTORY_COMPLETE_DAY_COUNT],
        uint8_t daytimeStartHour,
        uint8_t daytimeEndHour,
        const PoolCharacteristics& pool,
        PoolHistorySnapshot& out) const;

    const PoolHistoryDayState& todayState() const { return today_; }
    const PoolHistoryDayState& completedDayState(uint8_t index) const {
        return completedDays_[index < POOL_HISTORY_COMPLETE_DAY_COUNT ? index : 0U];
    }

private:
    static PoolHistoryDayState makeDay_(uint32_t localDate, uint64_t dayStartUtc);
    static void noteObservation_(PoolHistoryDayState& day, uint64_t observedAtUtc);
    static void fillMetricSummary_(const PoolHistoryMetricState& state,
                                   PoolHistoryMetricSummary& out);
    static void fillDaySummary_(const PoolHistoryDayState& state,
                                PoolHistoryDaySummary& out);
    static void addMetricValue_(PoolHistoryMetricState& state,
                                float value,
                                uint64_t observedAtUtc,
                                PoolHistoryDayState& day);
    const PoolHistoryDayState* findCompletedDate_(uint32_t localDate) const;

    PoolHistoryDayState today_{};
    PoolHistoryDayState completedDays_[POOL_HISTORY_COMPLETE_DAY_COUNT]{};
};

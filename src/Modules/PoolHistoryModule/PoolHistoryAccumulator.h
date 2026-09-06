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
};

enum class PoolHistoryDayTransition : uint8_t {
    None = 0,
    Initialized,
    AdvancedOneDay,
    Realigned
};

class PoolHistoryAccumulator {
public:
    void reset();

    /**
     * Restore up to two persisted records, retaining only the current local day
     * and its immediate predecessor.
     */
    void restoreForDate(uint32_t currentDate,
                        uint32_t previousDate,
                        uint64_t currentDayStartUtc,
                        const PoolHistoryDayState* first,
                        const PoolHistoryDayState* second);

    /** Align the active record with the supplied local date. */
    PoolHistoryDayTransition alignDay(uint32_t currentDate,
                                      uint32_t previousDate,
                                      uint64_t currentDayStartUtc);

    void addSample(PoolHistoryMetric metric, float value, uint64_t observedAtUtc);
    void observeFiltration(uint32_t intervalMs, bool running, uint64_t observedAtUtc);
    void snapshot(uint64_t generatedAtUtc, PoolHistorySnapshot& out) const;

    const PoolHistoryDayState& todayState() const { return today_; }
    const PoolHistoryDayState& previousDayState() const { return previousDay_; }

private:
    static PoolHistoryDayState makeDay_(uint32_t localDate, uint64_t dayStartUtc);
    static void noteObservation_(PoolHistoryDayState& day, uint64_t observedAtUtc);
    static void fillMetricSummary_(const PoolHistoryMetricState& state,
                                   PoolHistoryMetricSummary& out);
    static void fillDaySummary_(const PoolHistoryDayState& state,
                                PoolHistoryDaySummary& out);

    PoolHistoryDayState today_{};
    PoolHistoryDayState previousDay_{};
};

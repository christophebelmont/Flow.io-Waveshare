#pragma once
/**
 * @file IPoolHistory.h
 * @brief Read-only daily pool history service.
 */

#include <stdint.h>

#include "Core/Services/IPoolConfiguration.h"

constexpr uint8_t POOL_HISTORY_COMPLETE_DAY_COUNT = 7U;

/** @brief Aggregated values for one metric during one local calendar day. */
struct PoolHistoryMetricSummary {
    bool valid = false;
    uint32_t sampleCount = 0U;
    float first = 0.0f;
    float last = 0.0f;
    float minimum = 0.0f;
    float maximum = 0.0f;
    float average = 0.0f;
};

/** @brief Pool observations for one local calendar day. */
struct PoolHistoryDaySummary {
    bool valid = false;
    /** True once the local day is closed; false for the current day. */
    bool complete = false;
    /** Local date encoded as YYYYMMDD. */
    uint32_t localDate = 0U;
    /** UTC timestamp corresponding to local midnight for this day. */
    uint64_t dayStartUtc = 0U;
    /** Bounds of the observations actually collected by Flow.io. */
    uint64_t observedFromUtc = 0U;
    uint64_t observedUntilUtc = 0U;
    /** Actual filtration running time observed during the day. */
    bool filtrationRuntimeValid = false;
    uint32_t filtrationRunningSec = 0U;
    uint32_t filtrationRuntimeMinutes = 0U;
    float filtrationRuntimeHours = 0.0f;
    /** Total interval over which the filtration state was observed. */
    uint32_t filtrationObservedSec = 0U;
    PoolHistoryMetricSummary ph{};
    PoolHistoryMetricSummary orp{};
    PoolHistoryMetricSummary waterTemperature{};
    PoolHistoryMetricSummary airTemperature{};
    PoolHistoryMetricSummary daytimeWaterTemperature{};
    PoolHistoryMetricSummary nighttimeWaterTemperature{};
    /** Signed variation: nighttime average minus daytime average. */
    bool dayToNightTemperatureVariationValid = false;
    float dayToNightTemperatureVariationC = 0.0f;
    /** Refill volume is valid only when a positive fill-pump flow is configured. */
    bool refillVolumeValid = false;
    float refillVolumeLitres = 0.0f;
    bool refillEventsValid = false;
    uint32_t refillEventCount = 0U;
};

struct PoolHistoryPeriodSummary {
    uint8_t requestedDayCount = POOL_HISTORY_COMPLETE_DAY_COUNT;
    uint8_t availableDayCount = 0U;
    uint8_t filtrationAvailableDayCount = 0U;
    bool filtrationRuntimeValid = false;
    uint64_t totalFiltrationSec = 0U;
    uint32_t totalFiltrationMinutes = 0U;
    float totalFiltrationHours = 0.0f;
    bool averageDailyFiltrationValid = false;
    float averageDailyFiltrationHours = 0.0f;
    bool refillVolumeValid = false;
    float totalRefillVolumeLitres = 0.0f;
    float averageDailyRefillVolumeLitres = 0.0f;
    bool refillEventsValid = false;
    uint32_t totalRefillEventCount = 0U;
};

/** @brief Atomic history view intended for prompt preparation and UI consumers. */
struct PoolHistorySnapshot {
    uint64_t generatedAtUtc = 0U;
    uint8_t daytimeStartHour = 8U;
    uint8_t daytimeEndHour = 20U;
    PoolCharacteristics pool{};
    PoolHistoryDaySummary today{};
    /** Compatibility alias for completeDays[0]. */
    PoolHistoryDaySummary previousDay{};
    /** Previous seven complete local calendar days, newest first. */
    PoolHistoryDaySummary completeDays[POOL_HISTORY_COMPLETE_DAY_COUNT]{};
    PoolHistoryPeriodSummary last7Days{};
};

/** @brief Read-only service published by PoolHistoryModule. */
struct PoolHistoryService {
    bool (*getSnapshot)(void* ctx, PoolHistorySnapshot* outSnapshot);
    void* ctx;
};

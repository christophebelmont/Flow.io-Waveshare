#pragma once
/**
 * @file IPoolHistory.h
 * @brief Read-only daily pool history service.
 */

#include <stdint.h>

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
    uint32_t filtrationRunningSec = 0U;
    /** Total interval over which the filtration state was observed. */
    uint32_t filtrationObservedSec = 0U;
    PoolHistoryMetricSummary ph{};
    PoolHistoryMetricSummary orp{};
    PoolHistoryMetricSummary waterTemperature{};
    PoolHistoryMetricSummary airTemperature{};
};

/** @brief Atomic history view intended for prompt preparation and UI consumers. */
struct PoolHistorySnapshot {
    uint64_t generatedAtUtc = 0U;
    PoolHistoryDaySummary today{};
    PoolHistoryDaySummary previousDay{};
};

/** @brief Read-only service published by PoolHistoryModule. */
struct PoolHistoryService {
    bool (*getSnapshot)(void* ctx, PoolHistorySnapshot* outSnapshot);
    void* ctx;
};

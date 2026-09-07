#pragma once
/**
 * @file IAiInsight.h
 * @brief Weather context exposed by the staged pool insight feature.
 */

#include <stddef.h>
#include <stdint.h>

struct WeatherValueSummary {
    bool valid = false;
    float value = 0.0f;
};

struct WeatherRangeSummary {
    bool valid = false;
    uint16_t sampleCount = 0U;
    float minimum = 0.0f;
    float maximum = 0.0f;
};

struct WeatherAggregateSummary {
    bool valid = false;
    uint16_t sampleCount = 0U;
    float value = 0.0f;
};

struct PoolWeatherSnapshot {
    bool available = false;
    bool fromCache = false;
    double latitude = 0.0;
    double longitude = 0.0;
    uint64_t observedAtUtc = 0U;
    uint64_t fetchedAtUtc = 0U;
    uint32_t fetchedAtMs = 0U;

    WeatherValueSummary currentAirTemperatureC{};
    WeatherValueSummary currentCloudCoverPercent{};
    WeatherValueSummary currentWindSpeedKmh{};
    WeatherRangeSummary previous24hAirTemperatureC{};
    WeatherRangeSummary forecast24hAirTemperatureC{};
    WeatherAggregateSummary previous24hPrecipitationMm{};
    WeatherAggregateSummary forecast24hPrecipitationMm{};
    WeatherAggregateSummary forecast24hCloudCoverPercent{};
    WeatherAggregateSummary forecast24hMaximumWindSpeedKmh{};
    WeatherAggregateSummary forecast24hShortwaveRadiationWm2{};
};

enum class AiWeatherState : uint8_t {
    Idle = 0,
    Queued,
    Loading,
    Ready,
    Failed
};

struct AiWeatherStatus {
    AiWeatherState state = AiWeatherState::Idle;
    uint32_t updatedAtMs = 0U;
    PoolWeatherSnapshot weather{};
    char message[96]{};
};

struct AiInsightService {
    bool (*requestWeatherRefresh)(void* ctx,
                                  bool force,
                                  char* errOut,
                                  size_t errOutLen);
    bool (*getWeatherStatus)(void* ctx, AiWeatherStatus* outStatus);
    void* ctx;
};

#pragma once
/**
 * @file IAiInsight.h
 * @brief Weather context exposed by the staged pool insight feature.
 */

#include <stddef.h>
#include <stdint.h>

#include "Core/Services/IPoolHistory.h"

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

inline const char* aiWeatherStateCode(AiWeatherState state)
{
    switch (state) {
        case AiWeatherState::Idle: return "idle";
        case AiWeatherState::Queued: return "queued";
        case AiWeatherState::Loading: return "loading";
        case AiWeatherState::Ready: return "ready";
        case AiWeatherState::Failed: return "failed";
    }
    return "unknown";
}

struct AiWeatherStatus {
    AiWeatherState state = AiWeatherState::Idle;
    uint32_t updatedAtMs = 0U;
    PoolWeatherSnapshot weather{};
    char message[96]{};
};

enum class AiPoolInsightState : uint8_t {
    Idle = 0,
    Queued,
    Loading,
    Ready,
    Failed
};

inline const char* aiPoolInsightStateCode(AiPoolInsightState state)
{
    switch (state) {
        case AiPoolInsightState::Idle: return "idle";
        case AiPoolInsightState::Queued: return "queued";
        case AiPoolInsightState::Loading: return "loading";
        case AiPoolInsightState::Ready: return "ready";
        case AiPoolInsightState::Failed: return "failed";
    }
    return "unknown";
}

inline bool aiPoolInsightIsReusable(AiPoolInsightState state,
                                    uint64_t generatedAtUtc,
                                    uint64_t nowUtc,
                                    bool hasText,
                                    uint32_t lifetimeSec)
{
    return state == AiPoolInsightState::Ready && hasText &&
           generatedAtUtc > 0U && nowUtc >= generatedAtUtc &&
           (nowUtc - generatedAtUtc) < (uint64_t)lifetimeSec;
}

struct AiPoolInsightStatus {
    static constexpr size_t TextCapacity = 4096U;

    AiPoolInsightState state = AiPoolInsightState::Idle;
    uint32_t updatedAtMs = 0U;
    uint64_t generatedAtUtc = 0U;
    char message[256]{};
    char responseId[96]{};
    char model[48]{};
    char text[TextCapacity]{};
};

/**
 * @brief Text preview prepared locally before any OpenAI API call.
 *
 * This object is intentionally large. Runtime callers must allocate it in
 * PSRAM rather than on a task stack or in internal DRAM.
 */
struct AiPoolInsightPreview {
    static constexpr size_t WeatherTextCapacity = 1536U;
    static constexpr size_t PromptCapacity = 12288U;

    bool enabled = false;
    bool historyAvailable = false;
    PoolHistorySnapshot history{};
    AiWeatherState weatherState = AiWeatherState::Idle;
    AiPoolInsightState insightState = AiPoolInsightState::Idle;
    uint64_t insightGeneratedAtUtc = 0U;
    char model[48]{};
    char weatherMessage[96]{};
    char insightMessage[256]{};
    char insightText[AiPoolInsightStatus::TextCapacity]{};
    char weatherText[WeatherTextCapacity]{};
    char prompt[PromptCapacity]{};
};

struct AiInsightService {
    bool (*requestWeatherRefresh)(void* ctx,
                                  bool force,
                                  char* errOut,
                                  size_t errOutLen);
    bool (*getWeatherStatus)(void* ctx, AiWeatherStatus* outStatus);
    bool (*buildPoolPreview)(void* ctx,
                             AiPoolInsightPreview* outPreview,
                             char* errOut,
                             size_t errOutLen);
    bool (*requestPoolInsight)(void* ctx,
                               bool* outReused,
                               char* errOut,
                               size_t errOutLen);
    bool (*getPoolInsightStatus)(void* ctx,
                                 AiPoolInsightStatus* outStatus);
    void* ctx;
};

/**
 * @file OpenMeteoWeatherParser.cpp
 * @brief Strict parser for the bounded Open-Meteo request used by Flow.io.
 */

#include "Modules/AiInsightModule/OpenMeteoWeatherParser.h"

#include <math.h>
#include <stdio.h>

namespace {

constexpr size_t kMaximumHourlySamples = 64U;

bool writeError_(char* out, size_t outLen, const char* message)
{
    if (!out || outLen == 0U) return false;
    const int written = snprintf(out, outLen, "%s", message ? message : "weather parse failed");
    return written > 0 && (size_t)written < outLen;
}

bool readFiniteFloat_(JsonVariantConst value, float& out)
{
    if (value.isNull() ||
        !(value.is<float>() || value.is<double>() || value.is<int32_t>() ||
          value.is<uint32_t>())) {
        return false;
    }
    const float parsed = value.as<float>();
    if (!isfinite(parsed)) return false;
    out = parsed;
    return true;
}

void setValue_(WeatherValueSummary& summary, float value)
{
    summary.valid = true;
    summary.value = value;
}

void addRange_(WeatherRangeSummary& summary, float value)
{
    if (!summary.valid) {
        summary.valid = true;
        summary.minimum = value;
        summary.maximum = value;
    } else {
        if (value < summary.minimum) summary.minimum = value;
        if (value > summary.maximum) summary.maximum = value;
    }
    if (summary.sampleCount < UINT16_MAX) ++summary.sampleCount;
}

void addTotal_(WeatherAggregateSummary& summary, float value)
{
    if (!summary.valid) {
        summary.valid = true;
        summary.value = 0.0f;
    }
    summary.value += value;
    if (summary.sampleCount < UINT16_MAX) ++summary.sampleCount;
}

void addMaximum_(WeatherAggregateSummary& summary, float value)
{
    if (!summary.valid || value > summary.value) summary.value = value;
    summary.valid = true;
    if (summary.sampleCount < UINT16_MAX) ++summary.sampleCount;
}

void finishAverage_(WeatherAggregateSummary& summary)
{
    if (!summary.valid || summary.sampleCount == 0U) {
        summary = WeatherAggregateSummary{};
        return;
    }
    summary.value /= (float)summary.sampleCount;
}

}  // namespace

namespace OpenMeteoWeatherParser {

bool parse(JsonVariantConst root,
           double latitude,
           double longitude,
           uint64_t fetchedAtUtc,
           PoolWeatherSnapshot& out,
           char* errOut,
           size_t errOutLen)
{
    out = PoolWeatherSnapshot{};
    if (errOut && errOutLen > 0U) errOut[0] = '\0';
    if (!root.is<JsonObjectConst>()) {
        writeError_(errOut, errOutLen, "weather root is not an object");
        return false;
    }

    const JsonObjectConst current = root["current"].as<JsonObjectConst>();
    const JsonObjectConst hourly = root["hourly"].as<JsonObjectConst>();
    if (current.isNull() || hourly.isNull()) {
        writeError_(errOut, errOutLen, "weather response misses current or hourly data");
        return false;
    }

    const uint64_t observedAtUtc = current["time"] | 0ULL;
    float currentTemperature = 0.0f;
    float currentCloudCover = 0.0f;
    float currentWindSpeed = 0.0f;
    if (observedAtUtc == 0U ||
        !readFiniteFloat_(current["temperature_2m"], currentTemperature) ||
        !readFiniteFloat_(current["cloud_cover"], currentCloudCover) ||
        !readFiniteFloat_(current["wind_speed_10m"], currentWindSpeed)) {
        writeError_(errOut, errOutLen, "weather current data is incomplete");
        return false;
    }

    const JsonArrayConst times = hourly["time"].as<JsonArrayConst>();
    const JsonArrayConst temperatures = hourly["temperature_2m"].as<JsonArrayConst>();
    const JsonArrayConst precipitation = hourly["precipitation"].as<JsonArrayConst>();
    const JsonArrayConst cloudCover = hourly["cloud_cover"].as<JsonArrayConst>();
    const JsonArrayConst windSpeed = hourly["wind_speed_10m"].as<JsonArrayConst>();
    const JsonArrayConst shortwaveRadiation = hourly["shortwave_radiation"].as<JsonArrayConst>();
    const size_t sampleCount = times.size();
    if (times.isNull() || temperatures.isNull() || precipitation.isNull() ||
        cloudCover.isNull() || windSpeed.isNull() || shortwaveRadiation.isNull() ||
        sampleCount == 0U || sampleCount > kMaximumHourlySamples ||
        temperatures.size() != sampleCount || precipitation.size() != sampleCount ||
        cloudCover.size() != sampleCount || windSpeed.size() != sampleCount ||
        shortwaveRadiation.size() != sampleCount) {
        writeError_(errOut, errOutLen, "weather hourly arrays are invalid");
        return false;
    }

    out.latitude = latitude;
    out.longitude = longitude;
    out.observedAtUtc = observedAtUtc;
    out.fetchedAtUtc = fetchedAtUtc;
    setValue_(out.currentAirTemperatureC, currentTemperature);
    setValue_(out.currentCloudCoverPercent, currentCloudCover);
    setValue_(out.currentWindSpeedKmh, currentWindSpeed);

    for (size_t i = 0U; i < sampleCount; ++i) {
        const uint64_t timestamp = times[i] | 0ULL;
        if (timestamp == 0U) continue;
        const bool previousPeriod = timestamp <= observedAtUtc;

        float value = 0.0f;
        if (readFiniteFloat_(temperatures[i], value)) {
            addRange_(previousPeriod ? out.previous24hAirTemperatureC
                                     : out.forecast24hAirTemperatureC,
                      value);
        }
        if (readFiniteFloat_(precipitation[i], value)) {
            addTotal_(previousPeriod ? out.previous24hPrecipitationMm
                                     : out.forecast24hPrecipitationMm,
                      value);
        }
        if (!previousPeriod && readFiniteFloat_(cloudCover[i], value)) {
            addTotal_(out.forecast24hCloudCoverPercent, value);
        }
        if (!previousPeriod && readFiniteFloat_(windSpeed[i], value)) {
            addMaximum_(out.forecast24hMaximumWindSpeedKmh, value);
        }
        if (!previousPeriod && readFiniteFloat_(shortwaveRadiation[i], value)) {
            addTotal_(out.forecast24hShortwaveRadiationWm2, value);
        }
    }

    finishAverage_(out.forecast24hCloudCoverPercent);
    finishAverage_(out.forecast24hShortwaveRadiationWm2);
    if (!out.previous24hAirTemperatureC.valid ||
        !out.forecast24hAirTemperatureC.valid) {
        writeError_(errOut, errOutLen, "weather time ranges are incomplete");
        out = PoolWeatherSnapshot{};
        return false;
    }

    out.available = true;
    return true;
}

}  // namespace OpenMeteoWeatherParser

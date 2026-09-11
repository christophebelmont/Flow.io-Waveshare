/**
 * @file OpenMeteoWeatherParser.cpp
 * @brief Strict parser for current and daily Open-Meteo pool context.
 */

#include "Modules/AiInsightModule/OpenMeteoWeatherParser.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

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

void readOptionalValue_(JsonVariantConst value, WeatherValueSummary& out)
{
    float parsed = 0.0f;
    if (!readFiniteFloat_(value, parsed)) return;
    out.valid = true;
    out.value = parsed;
}

bool parseLocalDate_(const char* isoDateTime, uint32_t& outDate)
{
    outDate = 0U;
    if (!isoDateTime || strlen(isoDateTime) < 10U ||
        isoDateTime[4] != '-' || isoDateTime[7] != '-') return false;
    unsigned year = 0U;
    unsigned month = 0U;
    unsigned day = 0U;
    if (sscanf(isoDateTime, "%4u-%2u-%2u", &year, &month, &day) != 3 ||
        year < 2021U || year > 2199U || month < 1U || month > 12U ||
        day < 1U || day > 31U) return false;
    outDate = (uint32_t)year * 10000U + (uint32_t)month * 100U + (uint32_t)day;
    return true;
}

bool arraysAligned_(size_t expected,
                    JsonArrayConst minimumTemperature,
                    JsonArrayConst maximumTemperature,
                    JsonArrayConst meanTemperature,
                    JsonArrayConst precipitation,
                    JsonArrayConst cloudCover,
                    JsonArrayConst windSpeed,
                    JsonArrayConst radiation)
{
    return !minimumTemperature.isNull() && !maximumTemperature.isNull() &&
           !meanTemperature.isNull() && !precipitation.isNull() &&
           !cloudCover.isNull() && !windSpeed.isNull() && !radiation.isNull() &&
           minimumTemperature.size() == expected && maximumTemperature.size() == expected &&
           meanTemperature.size() == expected && precipitation.size() == expected &&
           cloudCover.size() == expected && windSpeed.size() == expected &&
           radiation.size() == expected;
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
    const JsonObjectConst daily = root["daily"].as<JsonObjectConst>();
    if (current.isNull() || daily.isNull()) {
        writeError_(errOut, errOutLen, "weather response misses current or daily data");
        return false;
    }

    uint32_t currentLocalDate = 0U;
    const char* currentTime = current["time"] | static_cast<const char*>(nullptr);
    float currentTemperature = 0.0f;
    float currentCloudCover = 0.0f;
    float currentWindSpeed = 0.0f;
    if (!parseLocalDate_(currentTime, currentLocalDate) ||
        !readFiniteFloat_(current["temperature_2m"], currentTemperature) ||
        !readFiniteFloat_(current["cloud_cover"], currentCloudCover) ||
        !readFiniteFloat_(current["wind_speed_10m"], currentWindSpeed)) {
        writeError_(errOut, errOutLen, "weather current data is incomplete");
        return false;
    }

    const JsonArrayConst dates = daily["time"].as<JsonArrayConst>();
    const JsonArrayConst minimumTemperature = daily["temperature_2m_min"].as<JsonArrayConst>();
    const JsonArrayConst maximumTemperature = daily["temperature_2m_max"].as<JsonArrayConst>();
    const JsonArrayConst meanTemperature = daily["temperature_2m_mean"].as<JsonArrayConst>();
    const JsonArrayConst precipitation = daily["precipitation_sum"].as<JsonArrayConst>();
    const JsonArrayConst cloudCover = daily["cloud_cover_mean"].as<JsonArrayConst>();
    const JsonArrayConst windSpeed = daily["wind_speed_10m_max"].as<JsonArrayConst>();
    const JsonArrayConst radiation = daily["shortwave_radiation_sum"].as<JsonArrayConst>();
    const size_t dayCount = dates.size();
    if (dates.isNull() || dayCount != POOL_WEATHER_DAILY_CAPACITY ||
        !arraysAligned_(dayCount, minimumTemperature, maximumTemperature,
                        meanTemperature, precipitation, cloudCover, windSpeed,
                        radiation)) {
        writeError_(errOut, errOutLen, "weather daily arrays are invalid");
        return false;
    }

    out.latitude = latitude;
    out.longitude = longitude;
    out.observedAtUtc = fetchedAtUtc;
    out.fetchedAtUtc = fetchedAtUtc;
    out.currentLocalDate = currentLocalDate;
    out.currentAirTemperatureC = {true, currentTemperature};
    out.currentCloudCoverPercent = {true, currentCloudCover};
    out.currentWindSpeedKmh = {true, currentWindSpeed};

    uint8_t historicalDayCount = 0U;
    uint32_t previousDate = 0U;
    for (size_t i = 0U; i < dayCount; ++i) {
        const char* dateText = dates[i] | static_cast<const char*>(nullptr);
        PoolWeatherDaySummary& day = out.daily[out.dailyCount];
        if (!parseLocalDate_(dateText, day.localDate) ||
            (previousDate != 0U && day.localDate <= previousDate)) {
            writeError_(errOut, errOutLen, "weather daily date is invalid");
            out = PoolWeatherSnapshot{};
            return false;
        }
        previousDate = day.localDate;
        day.valid = true;
        day.forecast = day.localDate >= currentLocalDate;
        if (!day.forecast) ++historicalDayCount;
        readOptionalValue_(minimumTemperature[i], day.minimumAirTemperatureC);
        readOptionalValue_(maximumTemperature[i], day.maximumAirTemperatureC);
        readOptionalValue_(meanTemperature[i], day.meanAirTemperatureC);
        readOptionalValue_(precipitation[i], day.precipitationMm);
        readOptionalValue_(cloudCover[i], day.meanCloudCoverPercent);
        readOptionalValue_(windSpeed[i], day.maximumWindSpeedKmh);
        readOptionalValue_(radiation[i], day.shortwaveRadiationMjM2);
        ++out.dailyCount;
    }

    if (historicalDayCount != 7U) {
        writeError_(errOut, errOutLen, "weather response does not contain seven past days");
        out = PoolWeatherSnapshot{};
        return false;
    }
    out.available = true;
    return true;
}

}  // namespace OpenMeteoWeatherParser

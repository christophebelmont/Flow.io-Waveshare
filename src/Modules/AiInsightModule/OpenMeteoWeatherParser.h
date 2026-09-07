#pragma once
/**
 * @file OpenMeteoWeatherParser.h
 * @brief Converts an Open-Meteo response into a bounded pool weather snapshot.
 */

#include "Core/Services/IAiInsight.h"

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

namespace OpenMeteoWeatherParser {

bool parse(JsonVariantConst root,
           double latitude,
           double longitude,
           uint64_t fetchedAtUtc,
           PoolWeatherSnapshot& out,
           char* errOut,
           size_t errOutLen);

}  // namespace OpenMeteoWeatherParser

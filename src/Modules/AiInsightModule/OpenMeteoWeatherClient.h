#pragma once
/**
 * @file OpenMeteoWeatherClient.h
 * @brief Bounded HTTPS client for the Open-Meteo forecast endpoint.
 */

#include "Core/Services/IAiInsight.h"

#include <stddef.h>
#include <stdint.h>

class OpenMeteoWeatherClient {
public:
    static constexpr size_t ResponseCapacity = 14U * 1024U;

    bool fetch(double latitude,
               double longitude,
               uint64_t fetchedAtUtc,
               char* responseBuffer,
               size_t responseCapacity,
               PoolWeatherSnapshot& out,
               char* errOut,
               size_t errOutLen) const;

private:
    static constexpr uint32_t kConnectTimeoutMs = 8000U;
    static constexpr uint32_t kRequestTimeoutMs = 15000U;
    static constexpr size_t kJsonCapacity = 16U * 1024U;
    static constexpr size_t kUrlCapacity = 512U;
};

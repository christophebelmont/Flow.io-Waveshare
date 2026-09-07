/**
 * @file OpenMeteoWeatherClient.cpp
 * @brief Fetches the weather window used by the future pool insight prompt.
 */

#include "Modules/AiInsightModule/OpenMeteoWeatherClient.h"

#include "Core/BoundedBufferStream.h"
#include "Modules/AiInsightModule/OpenMeteoWeatherParser.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>

namespace {

constexpr char kForecastUrlFormat[] =
    "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f"
    "&current=temperature_2m,cloud_cover,wind_speed_10m"
    "&hourly=temperature_2m,precipitation,cloud_cover,wind_speed_10m,shortwave_radiation"
    "&past_hours=24&forecast_hours=24&timeformat=unixtime&timezone=GMT";

struct SpiRamJsonAllocator {
    void* allocate(size_t size)
    {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    void deallocate(void* pointer)
    {
        heap_caps_free(pointer);
    }
};

using SpiRamJsonDocument = BasicJsonDocument<SpiRamJsonAllocator>;

bool writeError_(char* out, size_t outLen, const char* message)
{
    if (!out || outLen == 0U) return false;
    const int written = snprintf(out, outLen, "%s", message ? message : "weather request failed");
    return written > 0 && (size_t)written < outLen;
}

bool writeHttpError_(char* out, size_t outLen, int statusCode)
{
    if (!out || outLen == 0U) return false;
    const int written = snprintf(out, outLen, "weather HTTP status %d", statusCode);
    return written > 0 && (size_t)written < outLen;
}

bool locationIsValid_(double latitude, double longitude)
{
    return isfinite(latitude) && isfinite(longitude) &&
           latitude >= -90.0 && latitude <= 90.0 &&
           longitude >= -180.0 && longitude <= 180.0;
}

}  // namespace

bool OpenMeteoWeatherClient::fetch(double latitude,
                                   double longitude,
                                   uint64_t fetchedAtUtc,
                                   char* responseBuffer,
                                   size_t responseCapacity,
                                   PoolWeatherSnapshot& out,
                                   char* errOut,
                                   size_t errOutLen) const
{
    out = PoolWeatherSnapshot{};
    if (errOut && errOutLen > 0U) errOut[0] = '\0';
    if (!locationIsValid_(latitude, longitude)) {
        writeError_(errOut, errOutLen, "installation location is invalid");
        return false;
    }
    if (!responseBuffer || responseCapacity < (ResponseCapacity + 1U)) {
        writeError_(errOut, errOutLen, "weather response storage unavailable");
        return false;
    }

    char url[kUrlCapacity]{};
    const int urlLength = snprintf(url,
                                   sizeof(url),
                                   kForecastUrlFormat,
                                   latitude,
                                   longitude);
    if (urlLength <= 0 || (size_t)urlLength >= sizeof(url)) {
        writeError_(errOut, errOutLen, "weather URL is too long");
        return false;
    }

    NetworkClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(kConnectTimeoutMs);
    http.setTimeout(kRequestTimeoutMs);
    if (!http.begin(client, url)) {
        writeError_(errOut, errOutLen, "weather HTTP initialization failed");
        return false;
    }
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");

    const int statusCode = http.GET();
    if (statusCode != HTTP_CODE_OK) {
        writeHttpError_(errOut, errOutLen, statusCode);
        http.end();
        return false;
    }

    const int announcedSize = http.getSize();
    if (announcedSize == 0) {
        writeError_(errOut, errOutLen, "weather response is empty");
        http.end();
        return false;
    }
    if (announcedSize > 0 && (size_t)announcedSize > ResponseCapacity) {
        writeError_(errOut, errOutLen, "weather response is too large");
        http.end();
        return false;
    }

    BoundedBufferStream sink(responseBuffer, ResponseCapacity);
    const int written = http.writeToStream(&sink);
    const size_t payloadLength = sink.length();
    const bool overflowed = sink.overflowed();
    http.end();
    if (overflowed) {
        writeError_(errOut, errOutLen, "weather response is too large");
        return false;
    }
    if (written < 0 || payloadLength == 0U ||
        (announcedSize > 0 && payloadLength != (size_t)announcedSize)) {
        writeError_(errOut, errOutLen, "weather response read failed");
        return false;
    }
    responseBuffer[payloadLength] = '\0';

    SpiRamJsonDocument document(kJsonCapacity);
    if (document.capacity() < kJsonCapacity) {
        writeError_(errOut, errOutLen, "weather JSON storage unavailable");
        return false;
    }
    const DeserializationError jsonError =
        deserializeJson(document, responseBuffer, payloadLength);
    if (jsonError) {
        writeError_(errOut, errOutLen, "weather response contains invalid JSON");
        return false;
    }

    return OpenMeteoWeatherParser::parse(document.as<JsonVariantConst>(),
                                         latitude,
                                         longitude,
                                         fetchedAtUtc,
                                         out,
                                         errOut,
                                         errOutLen);
}

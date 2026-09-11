/**
 * @file OpenMeteoWeatherClient.cpp
 * @brief Fetches the weather window used by the future pool insight prompt.
 */

#include "Modules/AiInsightModule/OpenMeteoWeatherClient.h"

#include "Core/BoundedBufferStream.h"
#include "Core/LogModuleIds.h"
#include "Modules/AiInsightModule/OpenMeteoWeatherParser.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::AiInsightModule)
#include "Core/ModuleLog.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Network.h>
#include <NetworkClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdio.h>

namespace {

constexpr char kForecastHost[] = "api.open-meteo.com";
constexpr uint16_t kForecastPort = 443U;
constexpr char kForecastUrlFormat[] =
    "https://%s/v1/forecast?latitude=%.6f&longitude=%.6f"
    "&current=temperature_2m,cloud_cover,wind_speed_10m"
    "&daily=temperature_2m_min,temperature_2m_max,temperature_2m_mean,"
    "precipitation_sum,cloud_cover_mean,wind_speed_10m_max,shortwave_radiation_sum"
    "&past_days=7&forecast_days=2&timeformat=iso8601&timezone=auto";

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

void formatIpAddress_(const IPAddress& address, char* out, size_t outLen)
{
    if (!out || outLen == 0U) return;
    const String text = address.toString();
    snprintf(out, outLen, "%s", text.c_str());
}

void logTlsMemory_(const char* phase)
{
    const uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t totalCaps = MALLOC_CAP_8BIT;

    LOGI("Weather TLS memory phase=%s internal_free=%lu internal_largest=%lu",
         phase ? phase : "unknown",
         (unsigned long)heap_caps_get_free_size(internalCaps),
         (unsigned long)heap_caps_get_largest_free_block(internalCaps));
    LOGI("Weather TLS memory phase=%s psram_free=%lu psram_largest=%lu total_free=%lu total_largest=%lu stack_free_min=%u",
         phase ? phase : "unknown",
         (unsigned long)heap_caps_get_free_size(psramCaps),
         (unsigned long)heap_caps_get_largest_free_block(psramCaps),
         (unsigned long)heap_caps_get_free_size(totalCaps),
         (unsigned long)heap_caps_get_largest_free_block(totalCaps),
         (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

bool writeHttpFailure_(char* out,
                       size_t outLen,
                       int statusCode,
                       const char* httpError,
                       int tlsError)
{
    if (!out || outLen == 0U) return false;
    const int written = statusCode < 0
        ? snprintf(out,
                   outLen,
                   "weather HTTPS failed: %s (%d), transport error %d",
                   httpError ? httpError : "connection error",
                   statusCode,
                   tlsError)
        : snprintf(out, outLen, "weather HTTP status %d", statusCode);
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
                                   kForecastHost,
                                   latitude,
                                   longitude);
    if (urlLength <= 0 || (size_t)urlLength >= sizeof(url)) {
        writeError_(errOut, errOutLen, "weather URL is too long");
        return false;
    }

    NetworkInterface* defaultInterface = Network.getDefaultInterface();
    char localIp[48]{};
    char gatewayIp[48]{};
    char dnsIp[48]{};
    if (defaultInterface) {
        formatIpAddress_(defaultInterface->localIP(), localIp, sizeof(localIp));
        formatIpAddress_(defaultInterface->gatewayIP(), gatewayIp, sizeof(gatewayIp));
        formatIpAddress_(defaultInterface->dnsIP(), dnsIp, sizeof(dnsIp));
    }
    LOGI("Weather network online=%u default_if=%s local=%s gateway=%s dns=%s",
         Network.isOnline() ? 1U : 0U,
         defaultInterface && defaultInterface->ifkey() ? defaultInterface->ifkey() : "none",
         localIp[0] ? localIp : "none",
         gatewayIp[0] ? gatewayIp : "none",
         dnsIp[0] ? dnsIp : "none");

    IPAddress resolvedAddress;
    const int dnsResult = Network.hostByName(kForecastHost, resolvedAddress);
    if (dnsResult != 1) {
        if (errOut && errOutLen > 0U) {
            snprintf(errOut,
                     errOutLen,
                     "weather DNS failed for %s (error %d)",
                     kForecastHost,
                     dnsResult);
        }
        LOGE("Weather DNS failed host=%s result=%d default_if=%s dns=%s",
             kForecastHost,
             dnsResult,
             defaultInterface && defaultInterface->ifkey() ? defaultInterface->ifkey() : "none",
             dnsIp[0] ? dnsIp : "none");
        return false;
    }
    char resolvedIp[48]{};
    formatIpAddress_(resolvedAddress, resolvedIp, sizeof(resolvedIp));
    LOGI("Weather DNS resolved host=%s ip=%s port=%u",
         kForecastHost,
         resolvedIp,
         (unsigned)kForecastPort);

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

    logTlsMemory_("before");
    const int statusCode = http.GET();
    if (statusCode != HTTP_CODE_OK) {
        logTlsMemory_("failed-active");
        char tlsErrorText[128]{};
        const int tlsError = client.lastError(tlsErrorText, sizeof(tlsErrorText));
        const String httpErrorText = statusCode < 0
            ? HTTPClient::errorToString(statusCode)
            : String("HTTP response");
        writeHttpFailure_(errOut,
                          errOutLen,
                          statusCode,
                          httpErrorText.c_str(),
                          tlsError);
        LOGE("Weather HTTPS failed host=%s ip=%s port=%u http=%d (%s) transport=%d (%s)",
             kForecastHost,
             resolvedIp,
             (unsigned)kForecastPort,
             statusCode,
             httpErrorText.c_str(),
             tlsError,
             tlsErrorText[0] ? tlsErrorText : "no transport detail");
        http.end();
        logTlsMemory_("failed-cleanup");
        return false;
    }

    const int announcedSize = http.getSize();
    LOGI("Weather HTTP response host=%s status=%d announced_bytes=%d",
         kForecastHost,
         statusCode,
         announcedSize);
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
    logTlsMemory_("success-cleanup");
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

    const bool parsed = OpenMeteoWeatherParser::parse(document.as<JsonVariantConst>(),
                                                       latitude,
                                                       longitude,
                                                       fetchedAtUtc,
                                                       out,
                                                       errOut,
                                                       errOutLen);
    if (!parsed) {
        LOGE("Weather JSON parse failed bytes=%u detail=%s",
             (unsigned)payloadLength,
             errOut && errOut[0] ? errOut : "unknown");
        return false;
    }
    LOGI("Weather payload parsed bytes=%u observation_utc=%llu",
         (unsigned)payloadLength,
         (unsigned long long)out.observedAtUtc);
    return true;
}

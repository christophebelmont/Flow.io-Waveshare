/**
 * @file AiInsightModule.cpp
 * @brief Registers insight configuration and asynchronously refreshes weather data.
 */

#include "Modules/AiInsightModule/AiInsightModule.h"

#include "Core/CommandRegistry.h"
#include "Core/EventBus/EventPayloads.h"
#include "Core/LogModuleIds.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::AiInsightModule)
#include "Core/ModuleLog.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

struct AiInsightModule::Storage {
    char weatherResponse[OpenMeteoWeatherClient::ResponseCapacity + 1U]{};
    AiWeatherStatus weatherStatus{};
    bool refreshPending = false;
    bool forceRefresh = false;
};

namespace {

void writeOptionalValue_(JsonObject object,
                         const char* name,
                         const WeatherValueSummary& value)
{
    if (value.valid) object[name] = value.value;
    else object[name] = nullptr;
}

void writeRange_(JsonObject object,
                 const char* name,
                 const WeatherRangeSummary& range)
{
    JsonObject target = object.createNestedObject(name);
    target["valid"] = range.valid;
    target["samples"] = range.sampleCount;
    if (range.valid) {
        target["min"] = range.minimum;
        target["max"] = range.maximum;
    }
}

void writeAggregate_(JsonObject object,
                     const char* name,
                     const WeatherAggregateSummary& aggregate)
{
    JsonObject target = object.createNestedObject(name);
    target["valid"] = aggregate.valid;
    target["samples"] = aggregate.sampleCount;
    if (aggregate.valid) target["value"] = aggregate.value;
}

}  // namespace

AiInsightModule::~AiInsightModule()
{
    if (!storage_) return;
    storage_->~Storage();
    heap_caps_free(storage_);
    storage_ = nullptr;
}

bool AiInsightModule::writeError_(char* out, size_t outLen, const char* message)
{
    if (!out || outLen == 0U) return false;
    const int written = snprintf(out, outLen, "%s", message ? message : "failed");
    return written > 0 && (size_t)written < outLen;
}

bool AiInsightModule::locationIsValid_(double latitude, double longitude)
{
    return isfinite(latitude) && isfinite(longitude) &&
           latitude >= -90.0 && latitude <= 90.0 &&
           longitude >= -180.0 && longitude <= 180.0;
}

const char* AiInsightModule::weatherStateName_(AiWeatherState state)
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

bool AiInsightModule::requestWeatherRefresh_(bool force,
                                             char* errOut,
                                             size_t errOutLen)
{
    if (errOut && errOutLen > 0U) errOut[0] = '\0';
    if (!storage_) {
        writeError_(errOut, errOutLen, "weather storage unavailable");
        return false;
    }
    if (!cfgData_.enabled) {
        writeError_(errOut, errOutLen, "AI insight is disabled");
        return false;
    }
    if (!locationIsValid_(cfgData_.latitude, cfgData_.longitude)) {
        writeError_(errOut, errOutLen, "installation location is invalid");
        return false;
    }

    bool accepted = false;
    portENTER_CRITICAL(&lock_);
    if (!storage_->refreshPending &&
        storage_->weatherStatus.state != AiWeatherState::Loading) {
        storage_->refreshPending = true;
        storage_->forceRefresh = force;
        storage_->weatherStatus.state = AiWeatherState::Queued;
        storage_->weatherStatus.updatedAtMs = millis();
        memcpy(storage_->weatherStatus.message, "queued", sizeof("queued"));
        accepted = true;
    }
    portEXIT_CRITICAL(&lock_);
    if (!accepted) writeError_(errOut, errOutLen, "weather refresh already running");
    return accepted;
}

bool AiInsightModule::getWeatherStatus_(AiWeatherStatus* outStatus) const
{
    if (!storage_ || !outStatus) return false;
    portENTER_CRITICAL(&lock_);
    *outStatus = storage_->weatherStatus;
    portEXIT_CRITICAL(&lock_);
    return true;
}

uint64_t AiInsightModule::currentEpoch_() const
{
    if (!timeService_) return 0U;
    if (timeService_->currentState) {
        TimeState state{};
        if (timeService_->currentState(timeService_->ctx, &state) && state.valid) {
            return state.currentTimeUtc;
        }
    }
    return timeService_->epoch ? timeService_->epoch(timeService_->ctx) : 0U;
}

bool AiInsightModule::networkReady_() const
{
    return networkAccessService_ && networkAccessService_->isWebReachable &&
           networkAccessService_->isWebReachable(networkAccessService_->ctx);
}

void AiInsightModule::finishWeatherRequest_(AiWeatherState state,
                                            const PoolWeatherSnapshot* weather,
                                            const char* message)
{
    char boundedMessage[sizeof(storage_->weatherStatus.message)]{};
    snprintf(boundedMessage,
             sizeof(boundedMessage),
             "%s",
             message ? message : "");
    portENTER_CRITICAL(&lock_);
    storage_->weatherStatus.state = state;
    storage_->weatherStatus.updatedAtMs = millis();
    if (weather) storage_->weatherStatus.weather = *weather;
    memcpy(storage_->weatherStatus.message,
           boundedMessage,
           sizeof(storage_->weatherStatus.message));
    portEXIT_CRITICAL(&lock_);
}

void AiInsightModule::processWeatherRequest_()
{
    if (!storage_) return;

    bool pending = false;
    bool force = false;
    PoolWeatherSnapshot cached{};
    portENTER_CRITICAL(&lock_);
    if (storage_->refreshPending) {
        pending = true;
        force = storage_->forceRefresh;
        cached = storage_->weatherStatus.weather;
        storage_->refreshPending = false;
        storage_->forceRefresh = false;
        storage_->weatherStatus.state = AiWeatherState::Loading;
        storage_->weatherStatus.updatedAtMs = millis();
        memcpy(storage_->weatherStatus.message, "loading", sizeof("loading"));
    }
    portEXIT_CRITICAL(&lock_);
    if (!pending) return;

    const double latitude = cfgData_.latitude;
    const double longitude = cfgData_.longitude;
    const uint32_t nowMs = millis();
    const bool sameLocation = cached.available &&
                              fabs(cached.latitude - latitude) < 0.000001 &&
                              fabs(cached.longitude - longitude) < 0.000001;
    const bool cacheFresh = sameLocation && cached.fetchedAtMs != 0U &&
                            (uint32_t)(nowMs - cached.fetchedAtMs) < kCacheLifetimeMs;
    if (!force && cacheFresh) {
        cached.fromCache = true;
        finishWeatherRequest_(AiWeatherState::Ready, &cached, "ready (cache)");
        return;
    }
    if (!networkReady_()) {
        finishWeatherRequest_(AiWeatherState::Failed, nullptr, "network unavailable");
        return;
    }

    PoolWeatherSnapshot weather{};
    char error[96]{};
    if (!weatherClient_.fetch(latitude,
                              longitude,
                              currentEpoch_(),
                              storage_->weatherResponse,
                              sizeof(storage_->weatherResponse),
                              weather,
                              error,
                              sizeof(error))) {
        finishWeatherRequest_(AiWeatherState::Failed,
                              nullptr,
                              error[0] ? error : "weather refresh failed");
        LOGW("Weather refresh failed: %s", error[0] ? error : "unknown");
        return;
    }

    weather.fetchedAtMs = millis();
    weather.fromCache = false;
    finishWeatherRequest_(AiWeatherState::Ready, &weather, "ready");
    LOGI("Weather ready lat=%.4f lon=%.4f observed=%llu",
         latitude,
         longitude,
         (unsigned long long)weather.observedAtUtc);
}

bool AiInsightModule::buildWeatherStatusJson_(char* out, size_t outLen) const
{
    if (!out || outLen == 0U) return false;
    AiWeatherStatus status{};
    if (!getWeatherStatus_(&status)) return false;

    StaticJsonDocument<1536> document;
    document["ok"] = true;
    document["state"] = weatherStateName_(status.state);
    document["updated_at_ms"] = status.updatedAtMs;
    document["message"] = status.message;
    JsonObject weather = document.createNestedObject("weather");
    weather["available"] = status.weather.available;
    weather["from_cache"] = status.weather.fromCache;
    weather["latitude"] = status.weather.latitude;
    weather["longitude"] = status.weather.longitude;
    weather["observed_at_utc"] = status.weather.observedAtUtc;
    weather["fetched_at_utc"] = status.weather.fetchedAtUtc;
    writeOptionalValue_(weather, "current_temperature_c", status.weather.currentAirTemperatureC);
    writeOptionalValue_(weather, "current_cloud_cover_percent", status.weather.currentCloudCoverPercent);
    writeOptionalValue_(weather, "current_wind_speed_kmh", status.weather.currentWindSpeedKmh);
    writeRange_(weather, "previous_24h_temperature_c", status.weather.previous24hAirTemperatureC);
    writeRange_(weather, "forecast_24h_temperature_c", status.weather.forecast24hAirTemperatureC);
    writeAggregate_(weather, "previous_24h_precipitation_mm", status.weather.previous24hPrecipitationMm);
    writeAggregate_(weather, "forecast_24h_precipitation_mm", status.weather.forecast24hPrecipitationMm);
    writeAggregate_(weather, "forecast_24h_cloud_cover_percent", status.weather.forecast24hCloudCoverPercent);
    writeAggregate_(weather, "forecast_24h_max_wind_speed_kmh", status.weather.forecast24hMaximumWindSpeedKmh);
    writeAggregate_(weather, "forecast_24h_shortwave_radiation_wm2", status.weather.forecast24hShortwaveRadiationWm2);

    const size_t required = measureJson(document) + 1U;
    if (required > outLen) return false;
    return serializeJson(document, out, outLen) > 0U;
}

bool AiInsightModule::cmdWeatherRefresh_(void* userCtx,
                                         const CommandRequest&,
                                         char* reply,
                                         size_t replyLen)
{
    AiInsightModule* self = static_cast<AiInsightModule*>(userCtx);
    if (!self) return false;
    char error[96]{};
    if (!self->requestWeatherRefresh_(true, error, sizeof(error))) {
        snprintf(reply,
                 replyLen,
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error[0] ? error : "weather refresh rejected");
        return false;
    }
    snprintf(reply, replyLen, "{\"ok\":true,\"state\":\"queued\"}");
    return true;
}

bool AiInsightModule::cmdWeatherStatus_(void* userCtx,
                                        const CommandRequest&,
                                        char* reply,
                                        size_t replyLen)
{
    AiInsightModule* self = static_cast<AiInsightModule*>(userCtx);
    return self && self->buildWeatherStatusJson_(reply, replyLen);
}

void AiInsightModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    constexpr uint8_t kConfigModuleId = (uint8_t)ConfigModuleId::AiInsight;
    cfg.registerVar(enabledVar_, kConfigModuleId, kOpenAiConfigBranch);
    cfg.registerVar(apiKeyVar_, kConfigModuleId, kOpenAiConfigBranch);
    cfg.registerVar(modelVar_, kConfigModuleId, kOpenAiConfigBranch);
    cfg.registerVar(latitudeVar_, kConfigModuleId, kLocationConfigBranch);
    cfg.registerVar(longitudeVar_, kConfigModuleId, kLocationConfigBranch);

    void* storageMemory = heap_caps_malloc(sizeof(Storage),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storageMemory) storage_ = new (storageMemory) Storage{};
    if (!storage_) {
        LOGE("Weather storage allocation failed bytes=%u", (unsigned)sizeof(Storage));
    } else {
        snprintf(storage_->weatherStatus.message,
                 sizeof(storage_->weatherStatus.message),
                 "%s",
                 "idle");
        LOGI("Weather storage ready bytes=%u memory=psram", (unsigned)sizeof(Storage));
    }

    commandService_ = services.get<CommandService>(ServiceId::Command);
    networkAccessService_ = services.get<NetworkAccessService>(ServiceId::NetworkAccess);
    timeService_ = services.get<TimeService>(ServiceId::Time);
    if (!services.add(ServiceId::AiInsight, &service_)) {
        LOGE("Service registration failed: %s", toString(ServiceId::AiInsight));
    }
    if (commandService_ && commandService_->registerHandler) {
        if (!commandService_->registerHandler(commandService_->ctx,
                                              "ai.weather.refresh",
                                              &AiInsightModule::cmdWeatherRefresh_,
                                              this)) {
            LOGE("Could not register command ai.weather.refresh");
        }
        if (!commandService_->registerHandler(commandService_->ctx,
                                              "ai.weather.status",
                                              &AiInsightModule::cmdWeatherStatus_,
                                              this)) {
            LOGE("Could not register command ai.weather.status");
        }
    }
}

void AiInsightModule::onConfigLoaded(ConfigStore&, ServiceRegistry&)
{
    LOGI("Configured enabled=%u model=%s location_valid=%u",
         cfgData_.enabled ? 1U : 0U,
         cfgData_.model[0] ? cfgData_.model : "-",
         locationIsValid_(cfgData_.latitude, cfgData_.longitude) ? 1U : 0U);
}

void AiInsightModule::loop()
{
    processWeatherRequest_();
    vTaskDelay(pdMS_TO_TICKS(kLoopDelayMs));
}

/**
 * @file AiInsightModule.cpp
 * @brief Registers insight configuration and asynchronously refreshes weather data.
 */

#include "Modules/AiInsightModule/AiInsightModule.h"
#include "Modules/AiInsightModule/PoolInsightPromptBuilder.h"

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
    AiPoolInsightStatus insightStatus{};
    bool refreshPending = false;
    bool forceRefresh = false;
    bool insightPending = false;
};

namespace {

struct PoolInsightWork {
    AiPoolInsightPreview preview{};
    char apiKey[AiInsightConfig::ApiKeyCapacity]{};
    char configuredModel[AiInsightConfig::ModelCapacity]{};
    char resultText[AiPoolInsightStatus::TextCapacity]{};
};

void writeOptionalValue_(JsonObject object,
                         const char* name,
                         const WeatherValueSummary& value)
{
    if (value.valid) object[name] = value.value;
    else object[name] = nullptr;
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

bool AiInsightModule::requestPoolInsight_(bool* outReused,
                                          char* errOut,
                                          size_t errOutLen)
{
    if (outReused) *outReused = false;
    if (errOut && errOutLen > 0U) errOut[0] = '\0';
    if (!storage_) {
        writeError_(errOut, errOutLen, "pool insight storage unavailable");
        return false;
    }
    if (!cfgData_.enabled) {
        writeError_(errOut, errOutLen, "AI insight is disabled");
        return false;
    }
    if (cfgData_.apiKey[0] == '\0') {
        writeError_(errOut, errOutLen, "OpenAI API key is not configured");
        return false;
    }
    if (cfgData_.model[0] == '\0') {
        writeError_(errOut, errOutLen, "OpenAI model is not configured");
        return false;
    }
    if (!locationIsValid_(cfgData_.latitude, cfgData_.longitude)) {
        writeError_(errOut, errOutLen, "installation location is invalid");
        return false;
    }

    const uint64_t nowEpoch = currentEpoch_();
    uint64_t reusedAgeSec = 0U;
    bool reused = false;
    portENTER_CRITICAL(&lock_);
    const bool alreadyRunning = storage_->insightPending ||
        storage_->insightStatus.state == AiPoolInsightState::Loading;
    const bool reusable = !alreadyRunning && aiPoolInsightIsReusable(
        storage_->insightStatus.state,
        storage_->insightStatus.generatedAtUtc,
        nowEpoch,
        storage_->insightStatus.text[0] != '\0',
        kPoolInsightReuseLifetimeSec);
    if (reusable) {
        reused = true;
        reusedAgeSec = nowEpoch - storage_->insightStatus.generatedAtUtc;
    } else if (!alreadyRunning) {
        storage_->insightPending = true;
        storage_->insightStatus.state = AiPoolInsightState::Queued;
        storage_->insightStatus.updatedAtMs = millis();
        snprintf(storage_->insightStatus.message,
                 sizeof(storage_->insightStatus.message),
                 "%s",
                 "queued");
        storage_->insightStatus.generatedAtUtc = 0U;
        storage_->insightStatus.responseId[0] = '\0';
        storage_->insightStatus.model[0] = '\0';
        storage_->insightStatus.text[0] = '\0';
    }
    portEXIT_CRITICAL(&lock_);
    if (alreadyRunning) {
        writeError_(errOut, errOutLen, "pool insight request already running");
        return false;
    }
    if (reused) {
        if (outReused) *outReused = true;
        LOGI("Pool insight cache reused age_sec=%llu lifetime_sec=%lu",
             (unsigned long long)reusedAgeSec,
             (unsigned long)kPoolInsightReuseLifetimeSec);
        return true;
    }

    char weatherError[96]{};
    if (!requestWeatherRefresh_(false, weatherError, sizeof(weatherError))) {
        AiWeatherStatus weather{};
        const bool weatherInProgress = getWeatherStatus_(&weather) &&
            (weather.state == AiWeatherState::Queued ||
             weather.state == AiWeatherState::Loading);
        if (!weatherInProgress && weather.state != AiWeatherState::Ready) {
            finishPoolInsightRequest_(AiPoolInsightState::Failed,
                                      nullptr,
                                      nullptr,
                                      weatherError[0] ? weatherError : "weather refresh unavailable");
            writeError_(errOut,
                        errOutLen,
                        weatherError[0] ? weatherError : "weather refresh unavailable");
            return false;
        }
    }
    LOGI("Pool insight request queued model=%s", cfgData_.model);
    return true;
}

bool AiInsightModule::getWeatherStatus_(AiWeatherStatus* outStatus) const
{
    if (!storage_ || !outStatus) return false;
    portENTER_CRITICAL(&lock_);
    *outStatus = storage_->weatherStatus;
    portEXIT_CRITICAL(&lock_);
    return true;
}

bool AiInsightModule::getPoolInsightStatus_(AiPoolInsightStatus* outStatus) const
{
    if (!storage_ || !outStatus) return false;
    portENTER_CRITICAL(&lock_);
    *outStatus = storage_->insightStatus;
    portEXIT_CRITICAL(&lock_);
    return true;
}

bool AiInsightModule::buildPoolPreview_(AiPoolInsightPreview* outPreview,
                                        char* errOut,
                                        size_t errOutLen) const
{
    if (errOut && errOutLen > 0U) errOut[0] = '\0';
    if (!outPreview) {
        writeError_(errOut, errOutLen, "preview output is unavailable");
        return false;
    }
    memset(outPreview, 0, sizeof(*outPreview));
    outPreview->enabled = cfgData_.enabled;
    snprintf(outPreview->model, sizeof(outPreview->model), "%s", cfgData_.model);

    AiWeatherStatus weather{};
    if (!getWeatherStatus_(&weather)) {
        writeError_(errOut, errOutLen, "weather status is unavailable");
        return false;
    }
    outPreview->weatherState = weather.state;
    snprintf(outPreview->weatherMessage,
             sizeof(outPreview->weatherMessage),
             "%s",
             weather.message);

    portENTER_CRITICAL(&lock_);
    outPreview->insightState = storage_->insightStatus.state;
    outPreview->insightGeneratedAtUtc = storage_->insightStatus.generatedAtUtc;
    memcpy(outPreview->insightMessage,
           storage_->insightStatus.message,
           sizeof(outPreview->insightMessage));
    memcpy(outPreview->insightText,
           storage_->insightStatus.text,
           sizeof(outPreview->insightText));
    portEXIT_CRITICAL(&lock_);

    const bool historyAvailable = poolHistoryService_ && poolHistoryService_->getSnapshot &&
                                  poolHistoryService_->getSnapshot(poolHistoryService_->ctx,
                                                                  &outPreview->history);
    outPreview->historyAvailable = historyAvailable;
    if (!PoolInsightPromptBuilder::build(historyAvailable ? &outPreview->history : nullptr,
                                         weather,
                                         outPreview->weatherText,
                                         sizeof(outPreview->weatherText),
                                         outPreview->prompt,
                                         sizeof(outPreview->prompt))) {
        writeError_(errOut, errOutLen, "preview text exceeds its bounded capacity");
        return false;
    }
    const int instructionsLength = snprintf(outPreview->instructions,
                                            sizeof(outPreview->instructions),
                                            "%s",
                                            PoolInsightPromptBuilder::instructions());
    if (instructionsLength <= 0 ||
        (size_t)instructionsLength >= sizeof(outPreview->instructions)) {
        writeError_(errOut, errOutLen, "OpenAI instructions exceed bounded capacity");
        return false;
    }
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

void AiInsightModule::finishPoolInsightRequest_(
    AiPoolInsightState state,
    const OpenAiResponsesParser::Result* result,
    const char* text,
    const char* message)
{
    if (!storage_) return;
    char boundedMessage[sizeof(storage_->insightStatus.message)]{};
    char boundedModel[sizeof(storage_->insightStatus.model)]{};
    snprintf(boundedMessage,
             sizeof(boundedMessage),
             "%s",
             message ? message : "");
    const uint64_t generatedAtUtc = (result && text) ? currentEpoch_() : 0U;
    const size_t textLength = text
        ? strnlen(text, sizeof(storage_->insightStatus.text) - 1U)
        : 0U;
    if (result && text) {
        snprintf(boundedModel,
                 sizeof(boundedModel),
                 "%s",
                 result->model[0] ? result->model : cfgData_.model);
    }

    portENTER_CRITICAL(&lock_);
    storage_->insightPending = false;
    storage_->insightStatus.state = state;
    storage_->insightStatus.updatedAtMs = millis();
    memcpy(storage_->insightStatus.message,
           boundedMessage,
           sizeof(storage_->insightStatus.message));
    if (result && text) {
        storage_->insightStatus.generatedAtUtc = generatedAtUtc;
        memcpy(storage_->insightStatus.responseId,
               result->responseId,
               sizeof(storage_->insightStatus.responseId));
        memcpy(storage_->insightStatus.model,
               boundedModel,
               sizeof(storage_->insightStatus.model));
        memcpy(storage_->insightStatus.text, text, textLength);
        storage_->insightStatus.text[textLength] = '\0';
    }
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
                            (uint32_t)(nowMs - cached.fetchedAtMs) <
                                kWeatherCacheLifetimeMs;
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

void AiInsightModule::processPoolInsightRequest_()
{
    if (!storage_) return;

    AiWeatherState weatherState = AiWeatherState::Idle;
    char weatherMessage[sizeof(storage_->weatherStatus.message)]{};
    bool shouldRun = false;
    portENTER_CRITICAL(&lock_);
    if (storage_->insightPending) {
        weatherState = storage_->weatherStatus.state;
        memcpy(weatherMessage,
               storage_->weatherStatus.message,
               sizeof(weatherMessage));
        if (weatherState == AiWeatherState::Ready) {
            storage_->insightPending = false;
            storage_->insightStatus.state = AiPoolInsightState::Loading;
            storage_->insightStatus.updatedAtMs = millis();
            snprintf(storage_->insightStatus.message,
                     sizeof(storage_->insightStatus.message),
                     "%s",
                     "loading");
            shouldRun = true;
        }
    }
    portEXIT_CRITICAL(&lock_);

    if (weatherState == AiWeatherState::Failed) {
        char error[128]{};
        snprintf(error,
                 sizeof(error),
                 "weather unavailable: %s",
                 weatherMessage[0] ? weatherMessage : "unknown");
        finishPoolInsightRequest_(AiPoolInsightState::Failed,
                                  nullptr,
                                  nullptr,
                                  error);
        LOGW("Pool insight cancelled: %s", error);
        return;
    }
    if (!shouldRun) return;
    if (!networkReady_()) {
        finishPoolInsightRequest_(AiPoolInsightState::Failed,
                                  nullptr,
                                  nullptr,
                                  "network unavailable");
        return;
    }

    void* workMemory = heap_caps_calloc(1U,
                                        sizeof(PoolInsightWork),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!workMemory) {
        finishPoolInsightRequest_(AiPoolInsightState::Failed,
                                  nullptr,
                                  nullptr,
                                  "pool insight PSRAM work storage unavailable");
        return;
    }
    auto* work = static_cast<PoolInsightWork*>(workMemory);
    snprintf(work->apiKey, sizeof(work->apiKey), "%s", cfgData_.apiKey);
    snprintf(work->configuredModel,
             sizeof(work->configuredModel),
             "%s",
             cfgData_.model);

    char error[256]{};
    if (!buildPoolPreview_(&work->preview, error, sizeof(error))) {
        finishPoolInsightRequest_(AiPoolInsightState::Failed,
                                  nullptr,
                                  nullptr,
                                  error[0] ? error : "pool insight prompt unavailable");
        heap_caps_free(workMemory);
        return;
    }

    OpenAiResponsesParser::Result result{};
    const bool generated = openAiClient_.generate(work->apiKey,
                                                   work->configuredModel,
                                                   work->preview.instructions,
                                                   work->preview.prompt,
                                                   result,
                                                   work->resultText,
                                                   sizeof(work->resultText),
                                                   error,
                                                   sizeof(error));
    if (!generated) {
        finishPoolInsightRequest_(AiPoolInsightState::Failed,
                                  nullptr,
                                  nullptr,
                                  error[0] ? error : "OpenAI request failed");
        LOGW("Pool insight generation failed: %s", error[0] ? error : "unknown");
        heap_caps_free(workMemory);
        return;
    }

    finishPoolInsightRequest_(AiPoolInsightState::Ready,
                              &result,
                              work->resultText,
                              "ready");
    LOGI("Pool insight ready id=%s model=%s",
         result.responseId[0] ? result.responseId : "-",
         result.model[0] ? result.model : work->configuredModel);
    heap_caps_free(workMemory);
}

bool AiInsightModule::buildWeatherStatusJson_(char* out, size_t outLen) const
{
    if (!out || outLen == 0U) return false;
    AiWeatherStatus status{};
    if (!getWeatherStatus_(&status)) return false;

    StaticJsonDocument<4096> document;
    document["ok"] = true;
    document["state"] = aiWeatherStateCode(status.state);
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
    weather["current_local_date"] = status.weather.currentLocalDate;
    JsonArray daily = weather.createNestedArray("daily");
    for (uint8_t i = 0U; i < status.weather.dailyCount; ++i) {
        const PoolWeatherDaySummary& source = status.weather.daily[i];
        JsonObject day = daily.createNestedObject();
        day["valid"] = source.valid;
        day["local_date"] = source.localDate;
        day["forecast"] = source.forecast;
        writeOptionalValue_(day, "temperature_min_c", source.minimumAirTemperatureC);
        writeOptionalValue_(day, "temperature_max_c", source.maximumAirTemperatureC);
        writeOptionalValue_(day, "temperature_mean_c", source.meanAirTemperatureC);
        writeOptionalValue_(day, "precipitation_mm", source.precipitationMm);
        writeOptionalValue_(day, "cloud_cover_mean_percent", source.meanCloudCoverPercent);
        writeOptionalValue_(day, "wind_speed_max_kmh", source.maximumWindSpeedKmh);
        writeOptionalValue_(day, "shortwave_radiation_mj_m2", source.shortwaveRadiationMjM2);
    }

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
        snprintf(storage_->insightStatus.message,
                 sizeof(storage_->insightStatus.message),
                 "%s",
                 "idle");
        LOGI("AI insight storage ready bytes=%u memory=psram", (unsigned)sizeof(Storage));
    }

    commandService_ = services.get<CommandService>(ServiceId::Command);
    timeService_ = services.get<TimeService>(ServiceId::Time);
    poolHistoryService_ = services.get<PoolHistoryService>(ServiceId::PoolHistory);
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

void AiInsightModule::onConfigLoaded(ConfigStore&, ServiceRegistry& services)
{
    // NetworkAccess is published by the selected network provider from its own
    // onConfigLoaded() callback, after all module init() calls have completed.
    networkAccessService_ = services.get<NetworkAccessService>(ServiceId::NetworkAccess);
    if (!networkAccessService_) {
        LOGW("Network access service unavailable after configuration load");
    }
    LOGI("Configured enabled=%u api_key_configured=%u model=%s location_valid=%u",
         cfgData_.enabled ? 1U : 0U,
         cfgData_.apiKey[0] ? 1U : 0U,
         cfgData_.model[0] ? cfgData_.model : "-",
         locationIsValid_(cfgData_.latitude, cfgData_.longitude) ? 1U : 0U);
}

void AiInsightModule::loop()
{
    processWeatherRequest_();
    processPoolInsightRequest_();
    vTaskDelay(pdMS_TO_TICKS(kLoopDelayMs));
}

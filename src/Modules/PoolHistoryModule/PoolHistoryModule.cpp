/**
 * @file PoolHistoryModule.cpp
 * @brief Samples semantic pool values and keeps today/yesterday aggregates.
 */

#include "Modules/PoolHistoryModule/PoolHistoryModule.h"

#include "Core/LogModuleIds.h"
#include "Core/NvsKeys.h"
#include "Core/SystemLimits.h"
#include "Domain/Pool/PoolIds.h"
#include "Modules/PoolHistoryModule/PoolHistoryPersistence.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::PoolHistoryModule)
#include "Core/ModuleLog.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <new>
#include <time.h>

static_assert(PoolHistoryPersistence::EncodedSize <=
                  Limits::Config::Capacity::RuntimeBlobAsyncMax,
              "Pool history record exceeds ConfigStore async blob capacity");

struct PoolHistoryModule::Storage {
    PoolHistoryAccumulator history{};
    PoolHistoryDayState loadedToday{};
    PoolHistoryDayState loadedPreviousDay{};
    uint8_t persistenceScratch[PoolHistoryPersistence::EncodedSize]{};
    bool hasLoadedToday = false;
    bool hasLoadedPreviousDay = false;
    bool initialized = false;
    bool todayDirty = false;
    bool previousDayDirty = false;
    bool lastFiltrationKnown = false;
    bool lastFiltrationRunning = false;
    uint32_t lastTickMs = 0U;
    uint32_t lastTickDate = 0U;
    uint32_t lastMetricSampleMs = 0U;
    uint32_t lastPersistAttemptMs = 0U;
};

PoolHistoryModule::~PoolHistoryModule()
{
    if (!storage_) return;
    storage_->~Storage();
    heap_caps_free(storage_);
    storage_ = nullptr;
}

bool PoolHistoryModule::serviceGetSnapshot_(void* ctx,
                                            PoolHistorySnapshot* outSnapshot)
{
    PoolHistoryModule* self = static_cast<PoolHistoryModule*>(ctx);
    return self && outSnapshot && self->getSnapshot_(*outSnapshot);
}

bool PoolHistoryModule::lockState_(TickType_t timeoutTicks) const
{
    return stateMutex_ && xSemaphoreTake(stateMutex_, timeoutTicks) == pdTRUE;
}

void PoolHistoryModule::unlockState_() const
{
    if (stateMutex_) (void)xSemaphoreGive(stateMutex_);
}

bool PoolHistoryModule::getSnapshot_(PoolHistorySnapshot& outSnapshot) const
{
    if (!storage_ || !lockState_()) return false;
    uint64_t generatedAtUtc = 0U;
    (void)currentEpoch_(generatedAtUtc);
    storage_->history.snapshot(generatedAtUtc, outSnapshot);
    unlockState_();
    return true;
}

void PoolHistoryModule::init(ConfigStore&, ServiceRegistry& services)
{
    stateMutex_ = xSemaphoreCreateMutexStatic(&stateMutexBuffer_);
    void* storageMemory = heap_caps_malloc(sizeof(Storage),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storageMemory) {
        storage_ = new (storageMemory) Storage{};
    }
    configService_ = services.get<ConfigStoreService>(ServiceId::ConfigStore);
    timeService_ = services.get<TimeService>(ServiceId::Time);
    domainStatusService_ = services.get<DomainStatusService>(ServiceId::DomainStatus);

    if (!stateMutex_) LOGE("State mutex creation failed");
    if (!storage_) {
        LOGE("History unavailable: PSRAM allocation failed bytes=%u",
             (unsigned)sizeof(Storage));
    } else {
        LOGI("History storage ready bytes=%u memory=psram free_psram_kb=%lu",
             (unsigned)sizeof(Storage),
             (unsigned long)(ESP.getFreePsram() / 1024U));
    }
    if (!configService_) LOGE("Missing service: %s", toString(ServiceId::ConfigStore));
    if (!timeService_) LOGE("Missing service: %s", toString(ServiceId::Time));
    if (!domainStatusService_) LOGE("Missing service: %s", toString(ServiceId::DomainStatus));
    if (!services.add(ServiceId::PoolHistory, &service_)) {
        LOGE("Service registration failed: %s", toString(ServiceId::PoolHistory));
    }
}

void PoolHistoryModule::onConfigLoaded(ConfigStore& cfg, ServiceRegistry&)
{
    loadPersisted_(cfg);
}

void PoolHistoryModule::loadPersisted_(ConfigStore& cfg)
{
    if (!storage_) return;
    uint8_t* const encoded = storage_->persistenceScratch;
    size_t actualLength = 0U;

    if (cfg.readRuntimeBlob(NvsKeys::PoolHistory::Today,
                            encoded,
                            PoolHistoryPersistence::EncodedSize,
                            &actualLength) &&
        actualLength == PoolHistoryPersistence::EncodedSize &&
        PoolHistoryPersistence::decode(encoded, actualLength, storage_->loadedToday)) {
        storage_->hasLoadedToday = true;
    }

    actualLength = 0U;
    if (cfg.readRuntimeBlob(NvsKeys::PoolHistory::PreviousDay,
                            encoded,
                            PoolHistoryPersistence::EncodedSize,
                            &actualLength) &&
        actualLength == PoolHistoryPersistence::EncodedSize &&
        PoolHistoryPersistence::decode(encoded, actualLength, storage_->loadedPreviousDay)) {
        storage_->hasLoadedPreviousDay = true;
    }
}

bool PoolHistoryModule::currentEpoch_(uint64_t& outEpoch) const
{
    outEpoch = 0U;
    if (!timeService_) return false;

    if (timeService_->currentState) {
        TimeState state{};
        if (!timeService_->currentState(timeService_->ctx, &state) || !state.valid ||
            state.quality == TimeQuality::Invalid || state.currentTimeUtc < kMinimumValidEpoch) {
            return false;
        }
        outEpoch = state.currentTimeUtc;
        return true;
    }

    if (!timeService_->epoch) return false;
    const uint64_t epoch = timeService_->epoch(timeService_->ctx);
    if (epoch < kMinimumValidEpoch) return false;
    if (timeService_->isPlausible && !timeService_->isPlausible(epoch)) return false;
    outEpoch = epoch;
    return true;
}

bool PoolHistoryModule::localDayContext_(uint64_t epoch, LocalDayContext& out)
{
    const time_t currentEpoch = (time_t)epoch;
    struct tm localNow{};
    if (!localtime_r(&currentEpoch, &localNow)) return false;

    const uint32_t year = (uint32_t)(localNow.tm_year + 1900);
    const uint32_t month = (uint32_t)(localNow.tm_mon + 1);
    out.currentDate = (year * 10000UL) + (month * 100UL) + (uint32_t)localNow.tm_mday;

    struct tm localMidnight = localNow;
    localMidnight.tm_hour = 0;
    localMidnight.tm_min = 0;
    localMidnight.tm_sec = 0;
    localMidnight.tm_isdst = -1;
    const time_t midnightEpoch = mktime(&localMidnight);
    if (midnightEpoch < (time_t)kMinimumValidEpoch) return false;
    out.currentDayStartUtc = (uint64_t)midnightEpoch;

    struct tm previousNoon = localNow;
    previousNoon.tm_mday -= 1;
    previousNoon.tm_hour = 12;
    previousNoon.tm_min = 0;
    previousNoon.tm_sec = 0;
    previousNoon.tm_isdst = -1;
    const time_t previousEpoch = mktime(&previousNoon);
    if (previousEpoch < (time_t)kMinimumValidEpoch) return false;

    struct tm previousLocal{};
    if (!localtime_r(&previousEpoch, &previousLocal)) return false;
    const uint32_t previousYear = (uint32_t)(previousLocal.tm_year + 1900);
    const uint32_t previousMonth = (uint32_t)(previousLocal.tm_mon + 1);
    out.previousDate = (previousYear * 10000UL) +
                       (previousMonth * 100UL) +
                       (uint32_t)previousLocal.tm_mday;
    return true;
}

void PoolHistoryModule::initializeHistory_(const LocalDayContext& day,
                                           uint64_t nowEpoch,
                                           uint32_t nowMs)
{
    if (!storage_ || !lockState_()) return;
    storage_->history.restoreForDate(
        day.currentDate,
        day.previousDate,
        day.currentDayStartUtc,
        storage_->hasLoadedToday ? &storage_->loadedToday : nullptr,
        storage_->hasLoadedPreviousDay ? &storage_->loadedPreviousDay : nullptr);
    const PoolHistoryDayState restoredToday = storage_->history.todayState();
    const PoolHistoryDayState restoredPreviousDay = storage_->history.previousDayState();
    const bool currentRecordAlreadyStored =
        storage_->hasLoadedToday &&
        storage_->loadedToday.localDate == restoredToday.localDate &&
        storage_->loadedToday.observedUntilUtc == restoredToday.observedUntilUtc &&
        storage_->loadedToday.filtrationObservedMs == restoredToday.filtrationObservedMs;
    const bool previousRecordAlreadyStored =
        !restoredPreviousDay.valid ||
        (storage_->hasLoadedPreviousDay &&
         storage_->loadedPreviousDay.localDate == restoredPreviousDay.localDate &&
         storage_->loadedPreviousDay.observedUntilUtc == restoredPreviousDay.observedUntilUtc &&
         storage_->loadedPreviousDay.filtrationObservedMs == restoredPreviousDay.filtrationObservedMs);
    storage_->todayDirty = !currentRecordAlreadyStored;
    storage_->previousDayDirty = !previousRecordAlreadyStored;
    unlockState_();

    const bool restoredCurrentObservations = restoredToday.observedUntilUtc != 0U;
    const bool restoredPreviousRecord = restoredPreviousDay.valid;
    storage_->loadedToday = PoolHistoryDayState{};
    storage_->loadedPreviousDay = PoolHistoryDayState{};
    storage_->hasLoadedToday = false;
    storage_->hasLoadedPreviousDay = false;
    storage_->initialized = true;
    storage_->lastTickMs = nowMs;
    storage_->lastTickDate = day.currentDate;
    storage_->lastMetricSampleMs = nowMs - kMetricSamplePeriodMs;
    storage_->lastPersistAttemptMs = nowMs;
    storage_->lastFiltrationKnown =
        readFiltrationState_(storage_->lastFiltrationRunning);
    sampleMetrics_(nowEpoch,
                   nowMs,
                   storage_->lastFiltrationKnown,
                   storage_->lastFiltrationRunning);

    PoolHistorySnapshot snapshot{};
    if (getSnapshot_(snapshot)) {
        LOGI("Ready today=%lu previous=%lu restored_today=%u restored_previous=%u",
             (unsigned long)snapshot.today.localDate,
             (unsigned long)snapshot.previousDay.localDate,
             restoredCurrentObservations ? 1U : 0U,
             restoredPreviousRecord ? 1U : 0U);
    }
}

bool PoolHistoryModule::readFiltrationState_(bool& outRunning) const
{
    outRunning = false;
    if (!domainStatusService_ || !domainStatusService_->slotStatus) return false;
    DomainSlotStatus status{};
    if (!domainStatusService_->slotStatus(domainStatusService_->ctx,
                                          PoolIds::ActuatorFiltrationPump,
                                          &status) ||
        !status.hasPoolDevice) {
        return false;
    }
    outRunning = status.poolActualOn != 0U;
    return true;
}

bool PoolHistoryModule::readFloatSlot_(DomainSlotId slot,
                                       uint32_t nowMs,
                                       float& outValue) const
{
    outValue = 0.0f;
    if (!domainStatusService_ || !domainStatusService_->slotStatus) return false;
    DomainSlotStatus status{};
    if (!domainStatusService_->slotStatus(domainStatusService_->ctx, slot, &status) ||
        !status.active || !status.hasValue || !status.value.valid ||
        status.value.type != IO_VAL_FLOAT || !isfinite(status.value.v.f)) {
        return false;
    }
    if (status.value.tsMs == 0U || (uint32_t)(nowMs - status.value.tsMs) > kMaximumSensorAgeMs) {
        return false;
    }
    outValue = status.value.v.f;
    return true;
}

void PoolHistoryModule::sampleMetrics_(uint64_t nowEpoch,
                                       uint32_t nowMs,
                                       bool filtrationKnown,
                                       bool filtrationRunning)
{
    float ph = 0.0f;
    float orp = 0.0f;
    float waterTemperature = 0.0f;
    float airTemperature = 0.0f;
    const bool chemistryCanBeSampled = filtrationKnown && filtrationRunning;
    const bool hasPh = chemistryCanBeSampled && readFloatSlot_(PoolIds::SensorPh, nowMs, ph);
    const bool hasOrp = chemistryCanBeSampled && readFloatSlot_(PoolIds::SensorOrp, nowMs, orp);
    const bool hasWaterTemperature =
        readFloatSlot_(PoolIds::SensorWaterTemp, nowMs, waterTemperature);
    const bool hasAirTemperature = readFloatSlot_(PoolIds::SensorAirTemp, nowMs, airTemperature);

    if (!storage_ || !lockState_()) return;
    if (hasPh) storage_->history.addSample(PoolHistoryMetric::Ph, ph, nowEpoch);
    if (hasOrp) storage_->history.addSample(PoolHistoryMetric::Orp, orp, nowEpoch);
    if (hasWaterTemperature) {
        storage_->history.addSample(PoolHistoryMetric::WaterTemperature,
                                    waterTemperature,
                                    nowEpoch);
    }
    if (hasAirTemperature) {
        storage_->history.addSample(PoolHistoryMetric::AirTemperature, airTemperature, nowEpoch);
    }
    if (hasPh || hasOrp || hasWaterTemperature || hasAirTemperature) {
        storage_->todayDirty = true;
    }
    unlockState_();
    storage_->lastMetricSampleMs = nowMs;
}

bool PoolHistoryModule::persistDay_(const char* key,
                                    const PoolHistoryDayState& state) const
{
    if (!storage_ || !state.valid || !configService_ ||
        !configService_->writeRuntimeBlobAsync) {
        return false;
    }
    uint8_t* const encoded = storage_->persistenceScratch;
    size_t encodedLength = 0U;
    if (!PoolHistoryPersistence::encode(state,
                                        encoded,
                                        PoolHistoryPersistence::EncodedSize,
                                        encodedLength)) {
        LOGE("Could not encode history date=%lu", (unsigned long)state.localDate);
        return false;
    }
    return configService_->writeRuntimeBlobAsync(configService_->ctx,
                                                  key,
                                                  encoded,
                                                  encodedLength);
}

void PoolHistoryModule::persistIfDue_(uint32_t nowMs, bool force)
{
    if (!storage_) return;
    if (!force &&
        (uint32_t)(nowMs - storage_->lastPersistAttemptMs) < kPersistPeriodMs) {
        return;
    }
    storage_->lastPersistAttemptMs = nowMs;

    PoolHistoryDayState today{};
    PoolHistoryDayState previousDay{};
    bool persistToday = false;
    bool persistPreviousDay = false;
    if (!lockState_()) return;
    if (storage_->todayDirty) {
        today = storage_->history.todayState();
        persistToday = true;
    }
    if (storage_->previousDayDirty) {
        previousDay = storage_->history.previousDayState();
        persistPreviousDay = true;
    }
    unlockState_();

    if (persistPreviousDay) {
        if (persistDay_(NvsKeys::PoolHistory::PreviousDay, previousDay)) {
            storage_->previousDayDirty = false;
        } else {
            LOGW("Previous-day persistence deferred date=%lu",
                 (unsigned long)previousDay.localDate);
        }
    }
    if (persistToday) {
        if (persistDay_(NvsKeys::PoolHistory::Today, today)) {
            storage_->todayDirty = false;
        } else {
            LOGW("Current-day persistence deferred date=%lu", (unsigned long)today.localDate);
        }
    }
}

void PoolHistoryModule::loop()
{
    if (!storage_) {
        vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
        return;
    }
    const uint32_t nowMs = millis();
    uint64_t nowEpoch = 0U;
    LocalDayContext day{};
    if (!currentEpoch_(nowEpoch) || !localDayContext_(nowEpoch, day)) {
        vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
        return;
    }

    if (!storage_->initialized) {
        initializeHistory_(day, nowEpoch, nowMs);
        vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
        return;
    }

    const uint32_t intervalMs = (uint32_t)(nowMs - storage_->lastTickMs);
    const bool dateChanged = day.currentDate != storage_->lastTickDate;
    const bool sequentialDayChange =
        dateChanged && storage_->lastTickDate == day.previousDate;
    const bool intervalUsable = intervalMs <= kMaximumAccrualGapMs;

    if (!lockState_()) {
        vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
        return;
    }
    if (intervalUsable && (!dateChanged || sequentialDayChange) &&
        storage_->lastFiltrationKnown) {
        storage_->history.observeFiltration(intervalMs,
                                            storage_->lastFiltrationRunning,
                                            nowEpoch);
        storage_->todayDirty = true;
    }
    const PoolHistoryDayTransition transition =
        storage_->history.alignDay(day.currentDate,
                                   day.previousDate,
                                   day.currentDayStartUtc);
    if (transition == PoolHistoryDayTransition::AdvancedOneDay) {
        storage_->todayDirty = true;
        storage_->previousDayDirty = true;
    } else if (transition == PoolHistoryDayTransition::Realigned) {
        storage_->todayDirty = true;
        storage_->previousDayDirty = false;
    }
    unlockState_();

    if (!intervalUsable) {
        LOGW("Observation gap ignored duration_ms=%lu", (unsigned long)intervalMs);
    }
    if (transition != PoolHistoryDayTransition::None) {
        LOGI("Local day transition date=%lu kind=%u",
             (unsigned long)day.currentDate,
             (unsigned)transition);
    }

    storage_->lastTickMs = nowMs;
    storage_->lastTickDate = day.currentDate;
    storage_->lastFiltrationKnown =
        readFiltrationState_(storage_->lastFiltrationRunning);

    const bool sampleDue = transition != PoolHistoryDayTransition::None ||
                           (uint32_t)(nowMs - storage_->lastMetricSampleMs) >=
                               kMetricSamplePeriodMs;
    if (sampleDue) {
        sampleMetrics_(nowEpoch,
                       nowMs,
                       storage_->lastFiltrationKnown,
                       storage_->lastFiltrationRunning);
    }
    persistIfDue_(nowMs, transition != PoolHistoryDayTransition::None);
    vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
}

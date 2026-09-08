/**
 * @file PoolHistoryModule.cpp
 * @brief Samples semantic pool values and keeps today plus seven complete days.
 */

#include "Modules/PoolHistoryModule/PoolHistoryModule.h"

#include "Core/LogModuleIds.h"
#include "Core/EventBus/EventPayloads.h"
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
    PoolHistoryDayState loadedRecords[POOL_HISTORY_COMPLETE_DAY_COUNT + 2U]{};
    PoolHistoryDayState persistenceStateScratch{};
    uint8_t persistenceScratch[PoolHistoryPersistence::EncodedSize]{};
    uint8_t loadedRecordCount = 0U;
    bool initialized = false;
    bool todayDirty = false;
    bool completedDayDirty[POOL_HISTORY_COMPLETE_DAY_COUNT]{};
    PoolHistorySamplingGate waterQualityGate{};
    bool lastFillingKnown = false;
    bool lastFillingRunning = false;
    float lastFillingFlowLPerHour = 0.0f;
    uint32_t lastTickMs = 0U;
    uint32_t lastTickDate = 0U;
    uint32_t lastMetricSampleMs = 0U;
    uint32_t lastPersistAttemptMs = 0U;
};

namespace {

constexpr const char* kCompletedDayKeys[POOL_HISTORY_COMPLETE_DAY_COUNT] = {
    NvsKeys::PoolHistory::CompletedDay0,
    NvsKeys::PoolHistory::CompletedDay1,
    NvsKeys::PoolHistory::CompletedDay2,
    NvsKeys::PoolHistory::CompletedDay3,
    NvsKeys::PoolHistory::CompletedDay4,
    NvsKeys::PoolHistory::CompletedDay5,
    NvsKeys::PoolHistory::CompletedDay6,
};

}  // namespace

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
    if (!storage_) return false;
    uint64_t generatedAtUtc = 0U;
    LocalDayContext day{};
    if (!currentEpoch_(generatedAtUtc) || !localDayContext_(generatedAtUtc, day)) return false;
    PoolCharacteristics pool{};
    if (poolConfigurationService_ && poolConfigurationService_->getCharacteristics) {
        (void)poolConfigurationService_->getCharacteristics(
            poolConfigurationService_->ctx, &pool);
    }
    uint8_t daytimeStartHour = 0U;
    uint8_t daytimeEndHour = 0U;
    daytimePeriod_(daytimeStartHour, daytimeEndHour);
    if (!lockState_()) return false;
    storage_->history.snapshot(generatedAtUtc,
                               day.completeDates,
                               day.completeStartsUtc,
                               daytimeStartHour,
                               daytimeEndHour,
                               pool,
                               outSnapshot);
    unlockState_();
    return true;
}

void PoolHistoryModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    constexpr uint8_t kConfigModuleId = (uint8_t)ConfigModuleId::PoolHistory;
    constexpr uint8_t kPeriodsBranch = 1U;
    cfg.registerVar(daytimeStartHourVar_, kConfigModuleId, kPeriodsBranch);
    cfg.registerVar(daytimeEndHourVar_, kConfigModuleId, kPeriodsBranch);
    stateMutex_ = xSemaphoreCreateMutexStatic(&stateMutexBuffer_);
    void* storageMemory = heap_caps_malloc(sizeof(Storage),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storageMemory) {
        storage_ = new (storageMemory) Storage{};
    }
    configService_ = services.get<ConfigStoreService>(ServiceId::ConfigStore);
    timeService_ = services.get<TimeService>(ServiceId::Time);
    domainStatusService_ = services.get<DomainStatusService>(ServiceId::DomainStatus);
    poolConfigurationService_ =
        services.get<PoolConfigurationService>(ServiceId::PoolConfiguration);

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
    if (!poolConfigurationService_) {
        LOGE("Missing service: %s", toString(ServiceId::PoolConfiguration));
    }
    if (!services.add(ServiceId::PoolHistory, &service_)) {
        LOGE("Service registration failed: %s", toString(ServiceId::PoolHistory));
    }
}

void PoolHistoryModule::onConfigLoaded(ConfigStore& cfg, ServiceRegistry&)
{
    if (daytimeStartHour_ > 23U || daytimeEndHour_ > 23U ||
        daytimeStartHour_ == daytimeEndHour_) {
        LOGW("Invalid daytime period %u-%u, using %u-%u",
             (unsigned)daytimeStartHour_,
             (unsigned)daytimeEndHour_,
             (unsigned)kDefaultDayStartHour,
             (unsigned)kDefaultDayEndHour);
        daytimeStartHour_ = kDefaultDayStartHour;
        daytimeEndHour_ = kDefaultDayEndHour;
    }
    loadPersisted_(cfg);
}

void PoolHistoryModule::loadPersisted_(ConfigStore& cfg)
{
    if (!storage_) return;
    uint8_t* const encoded = storage_->persistenceScratch;
    const char* keys[POOL_HISTORY_COMPLETE_DAY_COUNT + 2U] = {
        NvsKeys::PoolHistory::Today,
        NvsKeys::PoolHistory::PreviousDay,
        NvsKeys::PoolHistory::CompletedDay0,
        NvsKeys::PoolHistory::CompletedDay1,
        NvsKeys::PoolHistory::CompletedDay2,
        NvsKeys::PoolHistory::CompletedDay3,
        NvsKeys::PoolHistory::CompletedDay4,
        NvsKeys::PoolHistory::CompletedDay5,
        NvsKeys::PoolHistory::CompletedDay6,
    };
    storage_->loadedRecordCount = 0U;
    for (uint8_t i = 0U; i < (uint8_t)(POOL_HISTORY_COMPLETE_DAY_COUNT + 2U); ++i) {
        size_t actualLength = 0U;
        PoolHistoryDayState decoded{};
        if (!cfg.readRuntimeBlob(keys[i], encoded, PoolHistoryPersistence::EncodedSize,
                                 &actualLength) ||
            !PoolHistoryPersistence::decode(encoded, actualLength, decoded)) {
            continue;
        }
        bool duplicate = false;
        for (uint8_t record = 0U; record < storage_->loadedRecordCount; ++record) {
            if (storage_->loadedRecords[record].localDate == decoded.localDate) {
                duplicate = true;
                if (decoded.observedUntilUtc >
                    storage_->loadedRecords[record].observedUntilUtc) {
                    storage_->loadedRecords[record] = decoded;
                }
                break;
            }
        }
        if (!duplicate &&
            storage_->loadedRecordCount < POOL_HISTORY_COMPLETE_DAY_COUNT + 2U) {
            storage_->loadedRecords[storage_->loadedRecordCount++] = decoded;
        }
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

    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        struct tm targetNoon = localNow;
        targetNoon.tm_mday -= (int)i + 1;
        targetNoon.tm_hour = 12;
        targetNoon.tm_min = 0;
        targetNoon.tm_sec = 0;
        targetNoon.tm_isdst = -1;
        const time_t targetEpoch = mktime(&targetNoon);
        if (targetEpoch < (time_t)kMinimumValidEpoch) return false;

        struct tm targetLocal{};
        if (!localtime_r(&targetEpoch, &targetLocal)) return false;
        const uint32_t targetYear = (uint32_t)(targetLocal.tm_year + 1900);
        const uint32_t targetMonth = (uint32_t)(targetLocal.tm_mon + 1);
        out.completeDates[i] = (targetYear * 10000UL) +
                               (targetMonth * 100UL) +
                               (uint32_t)targetLocal.tm_mday;
        targetLocal.tm_hour = 0;
        targetLocal.tm_min = 0;
        targetLocal.tm_sec = 0;
        targetLocal.tm_isdst = -1;
        const time_t targetMidnight = mktime(&targetLocal);
        if (targetMidnight < (time_t)kMinimumValidEpoch) return false;
        out.completeStartsUtc[i] = (uint64_t)targetMidnight;
    }
    return true;
}

void PoolHistoryModule::initializeHistory_(const LocalDayContext& day,
                                           uint64_t nowEpoch,
                                           uint32_t nowMs)
{
    if (!storage_ || !lockState_()) return;
    bool restoredTodayRecord = false;
    for (uint8_t i = 0U; i < storage_->loadedRecordCount; ++i) {
        if (storage_->loadedRecords[i].valid &&
            storage_->loadedRecords[i].localDate == day.currentDate) {
            restoredTodayRecord = true;
            break;
        }
    }
    storage_->history.restoreForDate(
        day.currentDate,
        day.currentDayStartUtc,
        day.completeDates,
        storage_->loadedRecords,
        storage_->loadedRecordCount);
    const PoolHistoryDayState restoredToday = storage_->history.todayState();
    storage_->todayDirty = !restoredTodayRecord;
    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        storage_->completedDayDirty[i] = false;
    }
    unlockState_();

    const bool restoredCurrentObservations = restoredToday.observedUntilUtc != 0U;
    uint8_t restoredCompleteCount = 0U;
    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        if (storage_->history.completedDayState(i).valid) ++restoredCompleteCount;
    }
    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT + 2U; ++i) {
        storage_->loadedRecords[i] = PoolHistoryDayState{};
    }
    storage_->loadedRecordCount = 0U;
    storage_->initialized = true;
    storage_->lastTickMs = nowMs;
    storage_->lastTickDate = day.currentDate;
    storage_->lastMetricSampleMs = nowMs - kMetricSamplePeriodMs;
    storage_->lastPersistAttemptMs = nowMs;
    bool filtrationRunning = false;
    const bool filtrationKnown = readFiltrationState_(filtrationRunning);
    storage_->waterQualityGate.initialize(filtrationKnown, filtrationRunning);
    storage_->lastFillingKnown = readFillingState_(storage_->lastFillingRunning,
                                                   storage_->lastFillingFlowLPerHour);
    sampleMetrics_(nowEpoch,
                   nowMs,
                   filtrationKnown,
                   filtrationRunning);

    LOGI("Ready today=%lu previous=%lu restored_today=%u restored_complete=%u",
         (unsigned long)restoredToday.localDate,
         (unsigned long)storage_->history.completedDayState(0U).localDate,
         restoredCurrentObservations ? 1U : 0U,
         (unsigned)restoredCompleteCount);
}

bool PoolHistoryModule::readFillingState_(bool& outRunning,
                                          float& outFlowLPerHour) const
{
    outRunning = false;
    outFlowLPerHour = 0.0f;
    if (!domainStatusService_ || !domainStatusService_->slotStatus) return false;
    DomainSlotStatus status{};
    if (!domainStatusService_->slotStatus(domainStatusService_->ctx,
                                          PoolIds::ActuatorFillPump,
                                          &status) ||
        !status.hasPoolDevice) {
        return false;
    }
    outRunning = status.poolActualOn != 0U;
    outFlowLPerHour = status.poolMeta.flowLPerHour;
    return true;
}

void PoolHistoryModule::daytimePeriod_(uint8_t& outStartHour,
                                       uint8_t& outEndHour) const
{
    outStartHour = daytimeStartHour_;
    outEndHour = daytimeEndHour_;
    if (outStartHour > 23U || outEndHour > 23U || outStartHour == outEndHour) {
        outStartHour = kDefaultDayStartHour;
        outEndHour = kDefaultDayEndHour;
    }
}

bool PoolHistoryModule::isDaytime_(uint64_t epoch) const
{
    const time_t sampleEpoch = (time_t)epoch;
    struct tm localSample{};
    if (!localtime_r(&sampleEpoch, &localSample)) return false;
    const uint8_t hour = (uint8_t)localSample.tm_hour;
    uint8_t startHour = 0U;
    uint8_t endHour = 0U;
    daytimePeriod_(startHour, endHour);
    if (startHour < endHour) {
        return hour >= startHour && hour < endHour;
    }
    return hour >= startHour || hour < endHour;
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
                                       uint32_t maximumAgeMs,
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
    if (status.value.tsMs == 0U ||
        (uint32_t)(nowMs - status.value.tsMs) > maximumAgeMs) {
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
    const bool waterQualityCanBeSampled =
        filtrationKnown && filtrationRunning && storage_ &&
        storage_->waterQualityGate.eligible(kWaterQualityWarmupMs);
    uint32_t waterQualityMaximumAgeMs = 0U;
    if (waterQualityCanBeSampled) {
        waterQualityMaximumAgeMs = storage_->waterQualityGate.maximumEligibleSampleAgeMs(
            kWaterQualityWarmupMs, kMaximumSensorAgeMs);
    }
    const bool hasPh = waterQualityCanBeSampled &&
        readFloatSlot_(PoolIds::SensorPh, nowMs, waterQualityMaximumAgeMs, ph);
    const bool hasOrp = waterQualityCanBeSampled &&
        readFloatSlot_(PoolIds::SensorOrp, nowMs, waterQualityMaximumAgeMs, orp);
    const bool hasWaterTemperature = waterQualityCanBeSampled &&
        readFloatSlot_(PoolIds::SensorWaterTemp, nowMs, waterQualityMaximumAgeMs,
                       waterTemperature);
    const bool hasAirTemperature = readFloatSlot_(PoolIds::SensorAirTemp, nowMs,
                                                  kMaximumSensorAgeMs, airTemperature);

    if (!storage_ || !lockState_()) return;
    if (hasPh) storage_->history.addSample(PoolHistoryMetric::Ph, ph, nowEpoch);
    if (hasOrp) storage_->history.addSample(PoolHistoryMetric::Orp, orp, nowEpoch);
    if (hasWaterTemperature) {
        storage_->history.addSample(PoolHistoryMetric::WaterTemperature,
                                    waterTemperature,
                                    nowEpoch);
        storage_->history.addWaterTemperatureSample(waterTemperature,
                                                    isDaytime_(nowEpoch),
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

    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        if (!storage_->completedDayDirty[i]) continue;
        if (!lockState_()) return;
        storage_->persistenceStateScratch = storage_->history.completedDayState(i);
        unlockState_();
        const PoolHistoryDayState& day = storage_->persistenceStateScratch;
        if (!day.valid) {
            storage_->completedDayDirty[i] = false;
            continue;
        }
        if (persistDay_(kCompletedDayKeys[i], day)) {
            storage_->completedDayDirty[i] = false;
        } else {
            LOGW("Completed-day persistence deferred index=%u date=%lu",
                 (unsigned)i,
                 (unsigned long)day.localDate);
            return;
        }
    }
    if (storage_->todayDirty) {
        if (!lockState_()) return;
        storage_->persistenceStateScratch = storage_->history.todayState();
        unlockState_();
        const PoolHistoryDayState& today = storage_->persistenceStateScratch;
        if (persistDay_(NvsKeys::PoolHistory::Today, today)) {
            storage_->todayDirty = false;
        } else {
            LOGW("Current-day persistence deferred date=%lu",
                 (unsigned long)today.localDate);
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
        dateChanged && storage_->lastTickDate == day.completeDates[0];
    const bool intervalUsable = intervalMs <= kMaximumAccrualGapMs;

    if (!lockState_()) {
        vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
        return;
    }
    if (intervalUsable && (!dateChanged || sequentialDayChange)) {
        if (storage_->waterQualityGate.known()) {
            storage_->history.observeFiltration(intervalMs,
                                                storage_->waterQualityGate.running(),
                                                nowEpoch);
            storage_->todayDirty = true;
        }
        storage_->waterQualityGate.accrue(intervalMs, true);
        if (storage_->lastFillingKnown) {
            storage_->history.observeRefill(intervalMs,
                                            storage_->lastFillingRunning,
                                            storage_->lastFillingFlowLPerHour,
                                            false,
                                            nowEpoch);
            storage_->todayDirty = true;
        }
    } else {
        storage_->waterQualityGate.accrue(intervalMs, false);
    }
    const PoolHistoryDayTransition transition =
        storage_->history.alignDay(day.currentDate,
                                   day.currentDayStartUtc,
                                   day.completeDates);
    if (transition != PoolHistoryDayTransition::None) {
        storage_->todayDirty = true;
        for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
            storage_->completedDayDirty[i] = true;
        }
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

    bool filtrationRunning = false;
    const bool filtrationKnown = readFiltrationState_(filtrationRunning);
    bool fillingRunning = false;
    float fillingFlowLPerHour = 0.0f;
    const bool fillingKnown = readFillingState_(fillingRunning, fillingFlowLPerHour);

    const bool refillStarted = fillingKnown && fillingRunning &&
        storage_->lastFillingKnown && !storage_->lastFillingRunning;
    if (refillStarted && lockState_()) {
        storage_->history.observeRefill(0U, false, fillingFlowLPerHour, true, nowEpoch);
        storage_->todayDirty = true;
        unlockState_();
    }
    storage_->waterQualityGate.update(filtrationKnown, filtrationRunning);

    storage_->lastTickMs = nowMs;
    storage_->lastTickDate = day.currentDate;
    storage_->lastFillingKnown = fillingKnown;
    storage_->lastFillingRunning = fillingRunning;
    storage_->lastFillingFlowLPerHour = fillingFlowLPerHour;

    const bool sampleDue = transition != PoolHistoryDayTransition::None ||
                           (uint32_t)(nowMs - storage_->lastMetricSampleMs) >=
                               kMetricSamplePeriodMs;
    if (sampleDue) {
        sampleMetrics_(nowEpoch, nowMs, filtrationKnown, filtrationRunning);
    }
    persistIfDue_(nowMs, transition != PoolHistoryDayTransition::None);
    vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
}

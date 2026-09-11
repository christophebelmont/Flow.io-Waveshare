#pragma once
/**
 * @file PoolHistoryModule.h
 * @brief Samples and persists compact daily pool history.
 */

#include "Core/Module.h"
#include "Core/ConfigTypes.h"
#include "Core/NvsKeys.h"
#include "Core/Services/IConfig.h"
#include "Core/Services/IDomainStatus.h"
#include "Core/Services/IPoolHistory.h"
#include "Core/Services/ITime.h"
#include "Modules/PoolHistoryModule/PoolHistoryAccumulator.h"

#include <freertos/semphr.h>

class PoolHistoryModule : public Module {
public:
    PoolHistoryModule() = default;
    ~PoolHistoryModule() override;
    PoolHistoryModule(const PoolHistoryModule&) = delete;
    PoolHistoryModule& operator=(const PoolHistoryModule&) = delete;

    ModuleId moduleId() const override { return ModuleId::PoolHistory; }
    const char* taskName() const override { return "poolhistory"; }
    uint8_t dependencyCount() const override { return 5U; }
    ModuleId dependency(uint8_t index) const override {
        if (index == 0U) return ModuleId::LogHub;
        if (index == 1U) return ModuleId::ConfigStore;
        if (index == 2U) return ModuleId::Time;
        if (index == 3U) return ModuleId::PoolDevice;
        if (index == 4U) return ModuleId::PoolLogic;
        return ModuleId::Unknown;
    }
    uint8_t taskCount() const override { return 1U; }
    const ModuleTaskSpec* taskSpecs() const override { return singleLoopTaskSpec(); }
    uint16_t taskStackSize() const override { return 4096U; }
    UBaseType_t taskStackCaps() const override {
        return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
    UBaseType_t taskPriority() const override { return 1U; }
    BaseType_t taskCore() const override { return 1; }
    uint32_t startDelayMs() const override { return 6000U; }

    void init(ConfigStore& cfg, ServiceRegistry& services) override;
    void onConfigLoaded(ConfigStore& cfg, ServiceRegistry& services) override;
    void loop() override;

private:
    struct Storage;

    struct LocalDayContext {
        uint32_t currentDate = 0U;
        uint64_t currentDayStartUtc = 0U;
        uint32_t completeDates[POOL_HISTORY_COMPLETE_DAY_COUNT]{};
        uint64_t completeStartsUtc[POOL_HISTORY_COMPLETE_DAY_COUNT]{};
    };

    static constexpr uint64_t kMinimumValidEpoch = 1609459200ULL;
    static constexpr uint32_t kLoopPeriodMs = 1000U;
    static constexpr uint32_t kMetricSamplePeriodMs = 5U * 60U * 1000U;
    static constexpr uint32_t kPersistPeriodMs = 60U * 60U * 1000U;
    static constexpr uint32_t kMaximumAccrualGapMs = 5U * 60U * 1000U;
    static constexpr uint32_t kMaximumSensorAgeMs = 15U * 60U * 1000U;
    static constexpr uint32_t kWaterQualityWarmupMs = 10U * 60U * 1000U;
    static constexpr uint8_t kDefaultDayStartHour = 8U;
    static constexpr uint8_t kDefaultDayEndHour = 20U;

    static bool serviceGetSnapshot_(void* ctx, PoolHistorySnapshot* outSnapshot);

    bool getSnapshot_(PoolHistorySnapshot& outSnapshot) const;
    bool lockState_(TickType_t timeoutTicks = pdMS_TO_TICKS(200U)) const;
    void unlockState_() const;
    bool currentEpoch_(uint64_t& outEpoch) const;
    static bool localDayContext_(uint64_t epoch, LocalDayContext& out);
    void loadPersisted_(ConfigStore& cfg);
    void initializeHistory_(const LocalDayContext& day, uint64_t nowEpoch, uint32_t nowMs);
    bool readFiltrationState_(bool& outRunning) const;
    bool readHeatingState_(bool& outRunning) const;
    bool readFillingState_(bool& outRunning, float& outFlowLPerHour) const;
    bool readFloatSlot_(DomainSlotId slot,
                        uint32_t nowMs,
                        uint32_t maximumAgeMs,
                        float& outValue) const;
    void daytimePeriod_(uint8_t& outStartHour, uint8_t& outEndHour) const;
    bool isDaytime_(uint64_t epoch) const;
    static PoolHistoryDayPeriod dayPeriod_(uint64_t epoch);
    void sampleMetrics_(uint64_t nowEpoch, uint32_t nowMs, bool filtrationKnown,
                        bool filtrationRunning);
    void persistIfDue_(uint32_t nowMs, bool force);
    bool persistDay_(const char* key, const PoolHistoryDayState& state) const;

    mutable StaticSemaphore_t stateMutexBuffer_{};
    mutable SemaphoreHandle_t stateMutex_ = nullptr;
    Storage* storage_ = nullptr;
    PoolHistoryService service_{&PoolHistoryModule::serviceGetSnapshot_, this};

    const ConfigStoreService* configService_ = nullptr;
    const DomainStatusService* domainStatusService_ = nullptr;
    const TimeService* timeService_ = nullptr;
    const PoolConfigurationService* poolConfigurationService_ = nullptr;

    uint8_t daytimeStartHour_ = kDefaultDayStartHour;
    uint8_t daytimeEndHour_ = kDefaultDayEndHour;
    ConfigVariable<uint8_t, 0> daytimeStartHourVar_{
        NVS_KEY(NvsKeys::PoolHistory::DayStartHour), "day_start_hour", "poolhistory/periods",
        ConfigType::UInt8, &daytimeStartHour_, ConfigPersistence::Persistent, 0U
    };
    ConfigVariable<uint8_t, 0> daytimeEndHourVar_{
        NVS_KEY(NvsKeys::PoolHistory::DayEndHour), "day_end_hour", "poolhistory/periods",
        ConfigType::UInt8, &daytimeEndHour_, ConfigPersistence::Persistent, 0U
    };
};

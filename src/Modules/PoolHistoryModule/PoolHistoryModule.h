#pragma once
/**
 * @file PoolHistoryModule.h
 * @brief Samples and persists compact daily pool history.
 */

#include "Core/Module.h"
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
    uint8_t dependencyCount() const override { return 4U; }
    ModuleId dependency(uint8_t index) const override {
        if (index == 0U) return ModuleId::LogHub;
        if (index == 1U) return ModuleId::ConfigStore;
        if (index == 2U) return ModuleId::Time;
        if (index == 3U) return ModuleId::PoolDevice;
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
        uint32_t previousDate = 0U;
        uint64_t currentDayStartUtc = 0U;
    };

    static constexpr uint64_t kMinimumValidEpoch = 1609459200ULL;
    static constexpr uint32_t kLoopPeriodMs = 1000U;
    static constexpr uint32_t kMetricSamplePeriodMs = 5U * 60U * 1000U;
    static constexpr uint32_t kPersistPeriodMs = 15U * 60U * 1000U;
    static constexpr uint32_t kMaximumAccrualGapMs = 5U * 60U * 1000U;
    static constexpr uint32_t kMaximumSensorAgeMs = 15U * 60U * 1000U;

    static bool serviceGetSnapshot_(void* ctx, PoolHistorySnapshot* outSnapshot);

    bool getSnapshot_(PoolHistorySnapshot& outSnapshot) const;
    bool lockState_(TickType_t timeoutTicks = pdMS_TO_TICKS(200U)) const;
    void unlockState_() const;
    bool currentEpoch_(uint64_t& outEpoch) const;
    static bool localDayContext_(uint64_t epoch, LocalDayContext& out);
    void loadPersisted_(ConfigStore& cfg);
    void initializeHistory_(const LocalDayContext& day, uint64_t nowEpoch, uint32_t nowMs);
    bool readFiltrationState_(bool& outRunning) const;
    bool readFloatSlot_(DomainSlotId slot, uint32_t nowMs, float& outValue) const;
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
};

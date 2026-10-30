#pragma once
/**
 * @file PoolDeviceModule.h
 * @brief Pool actuator domain layer above IOModule.
 *
 * Public facade only. The implementation is split across Lifecycle / Control /
 * Runtime / Commands translation units.
 */

#include "Core/Module.h"
#include "Core/RuntimeUi.h"
#include "Modules/Network/MQTTModule/MqttConfigRouteProducer.h"
#include "Core/RuntimeSnapshotProvider.h"
#include "Core/ServiceBinding.h"
#include "Core/Services/Services.h"
#include "Core/CommandRegistry.h"
#include "Core/ConfigTypes.h"
#include "Core/Services/ITime.h"
#include "Domain/DomainStatusServiceProvider.h"
#include "Domain/DomainTypes.h"
#include "Modules/PoolDeviceModule/PoolDeviceModuleDataModel.h"
#include <freertos/semphr.h>
#include "Drivers/PoolDeviceDriver.h"
#include "Drivers/PoolDriverConfig.h"
#include "Core/Control/TimedActuatorOverride.h"
#include "Core/Control/ActuatorOverrideDuration.h"

enum PoolDeviceType : uint8_t {
    POOL_DEVICE_FILTRATION = 0,
    POOL_DEVICE_PERISTALTIC = 1,
    POOL_DEVICE_RELAY_STD = 2
};

struct PoolDeviceDefinition {
    char label[24] = {0};
    uint8_t slot = 0xFF;
    /** Optional domain actuator slot used by this pool device command path. */
    DomainSlotId commandSlot = DOMAIN_SLOT_INVALID;
    /** Complete default driver descriptor, replaced by the configured descriptor at boot. */
    PoolDriverConfig control{};
    uint8_t type = POOL_DEVICE_RELAY_STD;
    bool enabled = true;
    float flowLPerHour = 0.0f;     // used for dosing volumes
    float tankCapacityMl = 0.0f;   // 0 means "not tracked"
    float tankInitialMl = 0.0f;    // <=0 means "use capacity"
    uint16_t dependsOnMask = 0;     // bit per pool-device slot
    int32_t maxUptimeDaySec = 0;   // 0 means "unlimited"
};

class PoolDeviceModule : public Module, public IRuntimeSnapshotProvider, public IRuntimeUiValueProvider {
public:
    ModuleId moduleId() const override { return ModuleId::PoolDevice; }
    ModuleId runtimeUiProviderModuleId() const override { return moduleId(); }
    const char* taskName() const override { return "pooldev"; }
    BaseType_t taskCore() const override { return 1; }
    uint8_t taskCount() const override { return 1; }
    const ModuleTaskSpec* taskSpecs() const override { return singleLoopTaskSpec(); }

    uint8_t dependencyCount() const override { return 6; }
    ModuleId dependency(uint8_t i) const override {
        if (i == 0) return ModuleId::LogHub;
        if (i == 1) return ModuleId::DataStore;
        if (i == 2) return ModuleId::Command;
        if (i == 3) return ModuleId::Time;
        if (i == 4) return ModuleId::Io;
        if (i == 5) return ModuleId::EventBus;
        return ModuleId::Unknown;
    }

    void init(ConfigStore& cfg, ServiceRegistry& services) override;
    void onConfigPrepared(ConfigStore& cfg, ServiceRegistry& services) override;
    void onConfigLoaded(ConfigStore& cfg, ServiceRegistry& services) override;
    void loop() override;
    uint16_t taskStackSize() const override { return 4096; }
    uint32_t startDelayMs() const override {
        return 5000U;
    }

    bool defineDevice(const PoolDeviceDefinition& def);
    void configureDomainStatus(const DomainSpec& domain,
                               bool (*bindingPortExists)(uint16_t bindingPort)) {
        domainStatusProvider_.configure(domain, bindingPortExists);
    }
    const char* deviceLabel(uint8_t idx) const;
    uint16_t runtimeSnapshotCount() const override;
    const char* runtimeSnapshotSuffix(uint16_t idx) const override;
    RuntimeRouteClass runtimeSnapshotClass(uint16_t idx) const override;
    bool runtimeSnapshotAffectsKey(uint16_t idx, DataKey key) const override;
    bool buildRuntimeSnapshot(uint16_t idx, char* out, size_t len, uint32_t& maxTsOut) const override;
    bool writeRuntimeUiValue(uint8_t valueId, IRuntimeUiWriter& writer) const override;

private:
    enum RuntimeUiValueId : uint8_t {
        RuntimeUiFiltrationOn = 1,
        RuntimeUiPhPumpOn = 2,
        RuntimeUiChlorinePumpOn = 3,
        RuntimeUiRobotOn = 4,
        RuntimeUiDeviceCount = 5,
    };

    static constexpr uint8_t RESET_PENDING_DAY = (1u << 0);
    static constexpr uint8_t RESET_PENDING_WEEK = (1u << 1);
    static constexpr uint8_t RESET_PENDING_MONTH = (1u << 2);
    static constexpr uint32_t RUNTIME_PERSIST_INTERVAL_MS = 60000U;
    static constexpr uint32_t MIN_VALID_EPOCH_SEC = 1609459200U; // 2021-01-01
    static constexpr size_t RUNTIME_PERSIST_BUF_LEN = 192U;

    struct PoolDeviceSlot {
        bool used = false;
        const char* id = nullptr;  // stable runtime id: pdN
        PoolDeviceDefinition def{};
        /** Cached IOServiceV2 endpoint id derived from the generic IO slot. */
        IoId ioId = IO_ID_INVALID;

        PoolDriverConfig driverConfig{};
        PoolDeviceDriver driver{};
        char driverJson[POOL_DRIVER_CONFIG_BYTES]{};
        char driverKey[16]{};
        char driverError[96]{};
        char haSuffix[24]{};
        char haTopic[40]{};
        char haCommand[160]{};
        char haOptions[160]{};
        char haModeSuffix[24]{};
        char haModeCommand[1024]{};
        char haModeOptions[512]{};
        char haTelemetrySuffix[32]{};
        bool driverReady = false;
        PoolDeviceTarget desired{};
        PoolDeviceTarget effective{};
        PoolDeviceFeedback feedback{};
        uint32_t revision = 0;
        ActuatorControlState control{};
        ActuatorOverridePolicy overridePolicy{};
        TimedActuatorOverride overrideTimer;
        PoolDeviceModule* owner = nullptr;
        char overrideKey[16]{};
        bool overrideBaseline = false;
        bool overrideRestored = false;
        uint64_t overrideRetryMs = 0;
        char overrideOption[48]{};
        bool desiredOn = false;
        bool actualOn = false;
        PoolInterlockState interlockState = PoolInterlockState::Ready;
        uint8_t blockReason = POOL_DEVICE_BLOCK_NONE;
        bool runtimePublishable = false;

        uint32_t lastTickMs = 0;
        uint64_t runningMsDay = 0;
        uint64_t runningMsWeek = 0;
        uint64_t runningMsMonth = 0;
        uint64_t runningMsTotal = 0;
        float injectedMlDay = 0.0f;
        float injectedMlWeek = 0.0f;
        float injectedMlMonth = 0.0f;
        float injectedMlTotal = 0.0f;
        uint32_t lastEquipmentPublishMs = 0;
        float tankRemainingL = 0.0f;
        uint32_t dayKey = 0;
        uint32_t weekKey = 0;
        uint32_t monthKey = 0;
        uint32_t stateTsMs = 0;
        uint32_t actualOnSinceMs = 0;
        uint32_t metricsTsMs = 0;
        uint32_t lastRuntimeCommitMs = 0;
        uint32_t lastPersistMs = 0;
        bool hasPersistedMetrics = false;
        bool persistDirty = false;
        bool persistImmediate = false;
        bool forceMetricsCommit = false;
    };

    struct PeriodKeys {
        uint32_t day = 0;
        uint32_t week = 0;
        uint32_t month = 0;
    };

    // Lifecycle
    static void onEventStatic_(const Event& e, void* user);
    void onEvent_(const Event& e);

    // Control
    uint8_t activeCount_() const;
    PoolDeviceSvcStatus svcMetaImpl_(uint8_t slot, PoolDeviceSvcMeta* outMeta) const;
    PoolDeviceSvcStatus svcReadActualOnImpl_(uint8_t slot, uint8_t* outOn, uint32_t* outTsMs) const;
    PoolDeviceSvcStatus svcSetOverridePoliciesImpl_(const ActuatorOverridePolicy* policies, uint8_t count);
    PoolDeviceSvcStatus svcReleaseOverrideImpl_(uint8_t slot, ActuatorOverrideReason reason);
    bool svcOverrideCommandImpl_(const CommandRequest&, char*, size_t, ActuatorOverrideCommand action);
    uint16_t svcOverrideDurationImpl_() const;
    void restoreOverrides_();
    void registerOverrideHa_();
    bool overrideSupported_(uint8_t slot) const;
    uint32_t overrideBinding_(uint8_t slot) const;
    bool overrideDependenciesSatisfied_(uint8_t slot) const;
    uint64_t overrideUtc_() const;
    bool resolveOverride_(uint8_t slot, bool safe, uint64_t now);
    static bool saveOverride_(void*, const uint8_t*, size_t);
    static bool saveOverrideDuration_(void*, const uint8_t*, size_t);
    static bool cmdOverride_(void*, const CommandRequest&, char*, size_t);
    static bool cmdOverrideOn_(void*, const CommandRequest&, char*, size_t);
    static bool cmdOverrideOff_(void*, const CommandRequest&, char*, size_t);
    static bool cmdOverrideRelease_(void*, const CommandRequest&, char*, size_t);
    static bool cmdOverrideDuration_(void*, const CommandRequest&, char*, size_t);
    static bool cmdOverrideSelect_(void*, const CommandRequest&, char*, size_t);
    bool buildOverrideSnapshot_(bool all, char*, size_t, uint32_t&) const;
    ActuatorOverrideDuration overrideDuration_;
    uint8_t overrideSelection_ = 0xFF;
    uint32_t overridePublishMs_ = 0;
    char overrideOptions_[1024]{};
    PoolDeviceSvcStatus svcSetRunningImpl_(uint8_t slot, uint8_t on);
    PoolDeviceSvcStatus svcSetManualRunningImpl_(uint8_t slot, uint8_t on);
    PoolDeviceSvcStatus setRunning_(uint8_t slot, uint8_t on, bool manual, const float* setpoint = nullptr);
    PoolDeviceSvcStatus svcSetRunningAtSetpointImpl_(uint8_t slot, uint8_t on, float setpoint);
    PoolDeviceSvcStatus setTarget_(uint8_t slot, const PoolDeviceTarget* target, bool manual);
    PoolDeviceSvcStatus svcSetWritesEnabledImpl_(uint8_t enabled);
    uint8_t svcWritesEnabledImpl_() const;
    PoolDeviceSvcStatus svcRefillTankImpl_(uint8_t slot, float remainingL);
    void tickDevices_(uint32_t nowMs, bool allowPersist = true);
    void resetDailyCounters_();
    void resetWeeklyCounters_();
    void resetMonthlyCounters_();
    void requestPeriodReconcile_();
    bool weekStartMondayFromConfig_() const;
    bool currentPeriodKeys_(PeriodKeys& out) const;
    bool reconcilePeriodCountersFromClock_();
    bool loadPersistedMetrics_(uint8_t slotIdx, PoolDeviceSlot& slot);
    bool persistMetrics_(uint8_t slotIdx, PoolDeviceSlot& slot, uint32_t nowMs);
    bool dependenciesSatisfied_(uint8_t slotIdx) const;
    void logStartInterlock_(uint8_t slotIdx, uint8_t reason) const;
    static bool maxUptimeReached_(const PoolDeviceSlot& slot);
    PoolDeviceSvcStatus svcSetTargetImpl_(uint8_t slot, const PoolDeviceTarget* target);
    PoolDeviceSvcStatus svcReadStateImpl_(uint8_t slot, PoolDeviceFeedback* state) const;
    bool configureDriver_(uint8_t slot);
    void registerDriverHa_(uint8_t slot);
    bool actuatorWritesEnabled() const { return writesEnabled_; }
    static uint32_t toSeconds_(uint64_t ms);

    // Runtime
    bool configureRuntime_();
    static MqttBuildResult buildCfgBasePdmStatic_(void* ctx, uint16_t messageId, MqttBuildContext& buildCtx);
    MqttBuildResult buildCfgBasePdm_(MqttBuildContext& buildCtx);
    bool snapshotRouteFromIndex_(uint8_t snapshotIdx, uint8_t& slotIdxOut, bool& metricsOut) const;
    bool slotRuntimePublishable_(uint8_t slotIdx) const;
    bool buildStateSnapshot_(uint8_t slotIdx, char* out, size_t len, uint32_t& maxTsOut) const;
    bool buildMetricsSnapshot_(uint8_t slotIdx, char* out, size_t len, uint32_t& maxTsOut) const;
    static const char* blockReasonStr_(uint8_t reason);

    // Commands
    enum class TargetField : uint8_t { Setpoint, Mode };
    bool handlePoolTarget_(const CommandRequest&, char* reply, size_t length, TargetField);
    static bool cmdPoolMode_(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen);
    static bool cmdPoolSetpoint_(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen);
    static bool cmdPoolWrite_(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen);
    static bool cmdPoolRefill_(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen);
    static bool cmdPoolResetUptime_(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen);
    static bool cmdPoolResetUptimeAll_(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen);
    bool handlePoolWrite_(const CommandRequest& req, char* reply, size_t replyLen);
    bool handlePoolRefill_(const CommandRequest& req, char* reply, size_t replyLen);
    bool handlePoolResetUptime_(const CommandRequest& req, char* reply, size_t replyLen);
    bool handlePoolResetUptimeAll_(const CommandRequest& req, char* reply, size_t replyLen);
    bool resetUptimeSlot_(uint8_t slot);
    uint8_t resetUptimeAll_();
    void emitActivity_(ActivityCode code,
                       ActivitySource source,
                       ActivitySeverity severity,
                       ActivityRole role,
                       ActivityState state,
                       ActivityReason reason,
                       uint8_t slot,
                       const char* title,
                       const char* detail,
                       const char* icon,
                       const Actor& actor) const;
    void emitAutoModeDisabledByManualActivity_(ActivityRole role,
                                               uint8_t slot,
                                               const char* autoLabel,
                                               const Actor& actor) const;
    ActivityRole activityRoleForSlot_(uint8_t slot) const;
    bool ensureStorage_();
    bool lockState_(TickType_t timeoutTicks = pdMS_TO_TICKS(200)) const;
    void unlockState_() const;
    size_t runtimePersistUsage_() const;
    size_t runtimePersistCapacity_() const { return (size_t)POOL_DEVICE_MAX * RUNTIME_PERSIST_BUF_LEN; }

    // State and configuration storage
    bool runtimeReady_ = false;
    bool writesEnabled_ = false;
    portMUX_TYPE resetMux_ = portMUX_INITIALIZER_UNLOCKED;
    mutable StaticSemaphore_t stateMutexBuf_{};
    mutable SemaphoreHandle_t stateMutex_ = nullptr;
    uint8_t resetPendingMask_ = 0;
    bool periodReconcilePending_ = true;

    char (*runtimePersistBuf_)[RUNTIME_PERSIST_BUF_LEN] = nullptr;
    PoolDeviceSlot* slots_ = nullptr;

    // Services and shared runtime integrations
    const LogHubService* logHub_ = nullptr;
    const IOServiceV2* ioSvc_ = nullptr;
    const ModbusMasterService* serialSvc_ = nullptr;
    const TimeService* timeSvc_ = nullptr;
    const CommandService* cmdSvc_ = nullptr;
    const MqttService* mqttSvc_ = nullptr;
    const HAService* haSvc_ = nullptr;
    const ActivityLogService* activityLogSvc_ = nullptr;
    PoolDeviceService poolSvc_{
        ServiceBinding::bind<&PoolDeviceModule::activeCount_>,
        ServiceBinding::bind<&PoolDeviceModule::svcMetaImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcReadActualOnImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcSetRunningImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcSetWritesEnabledImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcWritesEnabledImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcRefillTankImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcSetTargetImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcReadStateImpl_>,
        this,
        ServiceBinding::bind<&PoolDeviceModule::svcSetOverridePoliciesImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcReleaseOverrideImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcOverrideCommandImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcOverrideDurationImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcSetManualRunningImpl_>,
        ServiceBinding::bind<&PoolDeviceModule::svcSetRunningAtSetpointImpl_>
    };
    DomainStatusServiceProvider domainStatusProvider_{};
    EventBus* eventBus_ = nullptr;
    DataStore* dataStore_ = nullptr;
    ConfigStore* cfgStore_ = nullptr;
    const ConfigStoreService* cfgSvc_ = nullptr;
    MqttConfigRouteProducer* cfgMqttPub_ = nullptr;

    ConfigVariable<char,0>* cfgDriverVar_ = nullptr;
    ConfigVariable<bool,0>* cfgEnabledVar_ = nullptr;
    ConfigVariable<uint16_t,0>* cfgDependsVar_ = nullptr;
    ConfigVariable<float,0>* cfgFlowVar_ = nullptr;
    ConfigVariable<float,0>* cfgTankCapVar_ = nullptr;
    ConfigVariable<float,0>* cfgTankInitVar_ = nullptr;
    ConfigVariable<int32_t,0>* cfgMaxUptimeVar_ = nullptr;
};

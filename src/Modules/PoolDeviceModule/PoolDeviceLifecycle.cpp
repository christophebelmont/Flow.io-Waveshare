#include "Core/Values/SerialEquipmentValues.h"
/**
 * @file PoolDeviceLifecycle.cpp
 * @brief Lifecycle entry points and integration wiring for PoolDeviceModule.
 */

#include "PoolDeviceModule.h"
#include "Core/Values/PoolDeviceValues.h"
#include "Drivers/PoolSerialHa.h"
#include "Core/BufferUsageTracker.h"
#include "Core/MqttTopics.h"
#include "Domain/Pool/PoolIds.h"
#include "Domain/Pool/PoolDeviceSlots.h"
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::PoolDeviceModule)
#include "Core/ModuleLog.h"
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <new>
#include "Core/SpiRamJsonDocument.h"

namespace {
static constexpr uint8_t kPoolDeviceCfgProducerId = 48;
static constexpr const char* kPoolDeviceCfgTopicBase = "cfg/pdm";
static constexpr uint8_t kPoolDeviceCfgBranchBase = 1;
static constexpr uint8_t kTimeCfgBranch = 1;
static constexpr uint16_t kCfgMsgBasePdm = 1;
static constexpr uint16_t kCfgMsgPdmSlotBase = 16;

template <typename T>
T* allocPsramArray_(size_t count)
{
    void* mem = heap_caps_malloc(sizeof(T) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) mem = heap_caps_malloc(sizeof(T) * count, MALLOC_CAP_8BIT);
    if (!mem) return nullptr;

    T* out = static_cast<T*>(mem);
    for (size_t i = 0; i < count; ++i) {
        new (&out[i]) T();
    }
    return out;
}

template <size_t Cols>
char (*allocPsramCharTable_(size_t rows))[Cols]
{
    void* mem = heap_caps_calloc(rows, Cols, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) mem = heap_caps_calloc(rows, Cols, MALLOC_CAP_8BIT);
    return static_cast<char (*)[Cols]>(mem);
}

static constexpr uint8_t poolDeviceCfgBranchFromSlot_(uint8_t slot)
{
    return (slot < POOL_DEVICE_MAX) ? (uint8_t)(kPoolDeviceCfgBranchBase + slot) : 0;
}

} // namespace

bool PoolDeviceModule::ensureStorage_()
{
    if (!stateMutex_) {
        stateMutex_ = xSemaphoreCreateRecursiveMutexStatic(&stateMutexBuf_);
    }

    if (runtimePersistBuf_ && slots_ && cfgDriverVar_ && cfgEnabledVar_ && cfgDependsVar_ && cfgFlowVar_ &&
        cfgTankCapVar_ && cfgTankInitVar_ && cfgMaxUptimeVar_) {
        return stateMutex_ != nullptr;
    }

    if (!runtimePersistBuf_) {
        runtimePersistBuf_ = allocPsramCharTable_<RUNTIME_PERSIST_BUF_LEN>(POOL_DEVICE_MAX);
    }
    if (!slots_) slots_ = allocPsramArray_<PoolDeviceSlot>(POOL_DEVICE_MAX);
    if (!cfgDriverVar_) cfgDriverVar_ = allocPsramArray_<ConfigVariable<char,0>>(POOL_DEVICE_MAX);
    if (!cfgEnabledVar_) cfgEnabledVar_ = allocPsramArray_<ConfigVariable<bool,0>>(POOL_DEVICE_MAX);
    if (!cfgDependsVar_) cfgDependsVar_ = allocPsramArray_<ConfigVariable<uint16_t,0>>(POOL_DEVICE_MAX);
    if (!cfgFlowVar_) cfgFlowVar_ = allocPsramArray_<ConfigVariable<float,0>>(POOL_DEVICE_MAX);
    if (!cfgTankCapVar_) cfgTankCapVar_ = allocPsramArray_<ConfigVariable<float,0>>(POOL_DEVICE_MAX);
    if (!cfgTankInitVar_) cfgTankInitVar_ = allocPsramArray_<ConfigVariable<float,0>>(POOL_DEVICE_MAX);
    if (!cfgMaxUptimeVar_) cfgMaxUptimeVar_ = allocPsramArray_<ConfigVariable<int32_t,0>>(POOL_DEVICE_MAX);

    const bool ok = stateMutex_ && runtimePersistBuf_ && slots_ && cfgDriverVar_ && cfgEnabledVar_ && cfgDependsVar_ && cfgFlowVar_ &&
                    cfgTankCapVar_ && cfgTankInitVar_ && cfgMaxUptimeVar_;
    if (ok) {
        LOGI("PoolDevice scalable storage ready slots=%u slot_bytes=%u slots_psram=%u persist_bytes=%u",
             (unsigned)POOL_DEVICE_MAX,
             (unsigned)(POOL_DEVICE_MAX * sizeof(PoolDeviceSlot)),
             (unsigned)esp_ptr_external_ram(slots_),
             (unsigned)runtimePersistCapacity_());
    } else {
        LOGE("PoolDevice scalable storage allocation failed");
    }
    return ok;
}

bool PoolDeviceModule::lockState_(TickType_t timeoutTicks) const
{
    if (!stateMutex_) return false;
    return xSemaphoreTakeRecursive(stateMutex_, timeoutTicks) == pdTRUE;
}

void PoolDeviceModule::unlockState_() const
{
    if (stateMutex_) {
        (void)xSemaphoreGiveRecursive(stateMutex_);
    }
}

size_t PoolDeviceModule::runtimePersistUsage_() const
{
    if (!runtimePersistBuf_) return 0U;
    size_t total = 0U;
    for (uint8_t row = 0; row < POOL_DEVICE_MAX; ++row) {
        const size_t len = strnlen(runtimePersistBuf_[row], RUNTIME_PERSIST_BUF_LEN);
        if (len > 0U) total += len + 1U;
    }
    return total;
}

void PoolDeviceModule::onEventStatic_(const Event& e, void* user)
{
    if (!user) return;
    static_cast<PoolDeviceModule*>(user)->onEvent_(e);
}

void PoolDeviceModule::onEvent_(const Event& e)
{
    if (e.id == EventId::DataChanged) {
        if (!e.payload || e.len < sizeof(DataChangedPayload)) return;
        const DataChangedPayload* p = (const DataChangedPayload*)e.payload;
        if (p->id != DATAKEY_TIME_READY) return;
        if (!dataStore_) return;
        if (timeReady(*dataStore_)) requestPeriodReconcile_();
        return;
    }

    if (e.id == EventId::ConfigChanged) {
        if (!e.payload || e.len < sizeof(ConfigChangedPayload)) return;
        const ConfigChangedPayload* p = (const ConfigChangedPayload*)e.payload;
        if (p->moduleId == (uint8_t)ConfigModuleId::Time &&
            p->localBranchId == kTimeCfgBranch) {
            requestPeriodReconcile_();
        }
        return;
    }

    if (e.id != EventId::SchedulerEventTriggered) return;
    if (!e.payload || e.len < sizeof(SchedulerEventTriggeredPayload)) return;

    const SchedulerEventTriggeredPayload* p = (const SchedulerEventTriggeredPayload*)e.payload;
    if ((SchedulerEdge)p->edge != SchedulerEdge::Trigger) return;

    uint8_t pending = 0;
    if (p->eventId == TIME_EVENT_SYS_DAY_START) {
        pending = RESET_PENDING_DAY;
    } else if (p->eventId == TIME_EVENT_SYS_WEEK_START) {
        pending = RESET_PENDING_WEEK;
    } else if (p->eventId == TIME_EVENT_SYS_MONTH_START) {
        pending = RESET_PENDING_MONTH;
    } else {
        return;
    }

    portENTER_CRITICAL(&resetMux_);
    resetPendingMask_ |= pending;
    portEXIT_CRITICAL(&resetMux_);
}

void PoolDeviceModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    constexpr uint8_t kCfgModuleId = (uint8_t)ConfigModuleId::PoolDevice;
    if (!ensureStorage_()) return;

    cfgStore_ = &cfg;
    cfgSvc_ = services.get<ConfigStoreService>(ServiceId::ConfigStore);
    logHub_ = services.get<LogHubService>(ServiceId::LogHub);
    mqttSvc_ = services.get<MqttService>(ServiceId::Mqtt);
    ioSvc_ = services.get<IOServiceV2>(ServiceId::Io);
    serialSvc_ = services.get<ModbusMasterService>(ServiceId::ModbusMaster);
    timeSvc_ = services.get<TimeService>(ServiceId::Time);
    if (!services.add(ServiceId::PoolDevice, &poolSvc_)) {
        LOGE("service registration failed: %s", toString(ServiceId::PoolDevice));
    }
    domainStatusProvider_.bindServices(ioSvc_, &poolSvc_);
    if (!services.add(ServiceId::DomainStatus, &domainStatusProvider_.service())) {
        LOGE("service registration failed: %s", toString(ServiceId::DomainStatus));
    }
    cmdSvc_ = services.get<CommandService>(ServiceId::Command);
    haSvc_ = services.get<HAService>(ServiceId::Ha);
    activityLogSvc_ = services.get<ActivityLogService>(ServiceId::ActivityLog);
    const EventBusService* ebSvc = services.get<EventBusService>(ServiceId::EventBus);
    eventBus_ = ebSvc ? ebSvc->bus : nullptr;
    const DataStoreService* dsSvc = services.get<DataStoreService>(ServiceId::DataStore);
    dataStore_ = dsSvc ? dsSvc->store : nullptr;

    if (!ioSvc_) {
        LOGW("PoolDevice waiting for IOServiceV2");
    }

    if (eventBus_) {
        eventBus_->subscribe(EventId::SchedulerEventTriggered, &PoolDeviceModule::onEventStatic_, this);
        eventBus_->subscribe(EventId::DataChanged, &PoolDeviceModule::onEventStatic_, this);
        eventBus_->subscribe(EventId::ConfigChanged, &PoolDeviceModule::onEventStatic_, this);
    }

    // Each slot owns one config branch; runtime metrics use ConfigStore runtime blob APIs.
    for (uint8_t i = 0; i < POOL_DEVICE_MAX; ++i) {
        PoolDeviceSlot& s = slots_[i];
        if (!s.used) continue;
        const uint8_t localBranchId = poolDeviceCfgBranchFromSlot_(i);

        const PoolDeviceSlotDescriptor& slot = PoolDeviceSlots::kSlots[i];

        snprintf(s.driverKey, sizeof(s.driverKey), "act%u_driver", unsigned(i));
        cfgDriverVar_[i].nvsKey = s.driverKey;
        cfgDriverVar_[i].jsonName = "driver";
        cfgDriverVar_[i].moduleName = slot.configModuleName;
        cfgDriverVar_[i].type = ConfigType::CharArray;
        cfgDriverVar_[i].value = s.driverJson;
        cfgDriverVar_[i].persistence = ConfigPersistence::Persistent;
        cfgDriverVar_[i].size = sizeof(s.driverJson);
        cfgDriverVar_[i].validateText = [](const char* text) {
            PoolDriverConfig config{};
            return parsePoolDriverConfig(text, config, nullptr, 0);
        };
        cfg.registerVar(cfgDriverVar_[i], kCfgModuleId, localBranchId);

        cfgEnabledVar_[i].nvsKey = slot.enabledKey;
        cfgEnabledVar_[i].jsonName = "enabled";
        cfgEnabledVar_[i].moduleName = slot.configModuleName;
        cfgEnabledVar_[i].type = ConfigType::Bool;
        cfgEnabledVar_[i].value = &s.def.enabled;
        cfgEnabledVar_[i].persistence = ConfigPersistence::Persistent;
        cfgEnabledVar_[i].size = 0;
        cfg.registerVar(cfgEnabledVar_[i], kCfgModuleId, localBranchId);

        cfgDependsVar_[i].nvsKey = slot.dependsKey;
        cfgDependsVar_[i].jsonName = "depends_on_mask";
        cfgDependsVar_[i].moduleName = slot.configModuleName;
        cfgDependsVar_[i].type = ConfigType::UInt16;
        cfgDependsVar_[i].value = &s.def.dependsOnMask;
        cfgDependsVar_[i].persistence = ConfigPersistence::Persistent;
        cfgDependsVar_[i].size = 0;
        cfg.registerVar(cfgDependsVar_[i], kCfgModuleId, localBranchId);

        cfgFlowVar_[i].nvsKey = slot.flowKey;
        cfgFlowVar_[i].jsonName = "flow_l_h";
        cfgFlowVar_[i].moduleName = slot.configModuleName;
        cfgFlowVar_[i].type = ConfigType::Float;
        cfgFlowVar_[i].value = &s.def.flowLPerHour;
        cfgFlowVar_[i].persistence = ConfigPersistence::Persistent;
        cfgFlowVar_[i].size = 0;
        cfg.registerVar(cfgFlowVar_[i], kCfgModuleId, localBranchId);

        cfgTankCapVar_[i].nvsKey = slot.tankCapKey;
        cfgTankCapVar_[i].jsonName = "tank_cap_ml";
        cfgTankCapVar_[i].moduleName = slot.configModuleName;
        cfgTankCapVar_[i].type = ConfigType::Float;
        cfgTankCapVar_[i].value = &s.def.tankCapacityMl;
        cfgTankCapVar_[i].persistence = ConfigPersistence::Persistent;
        cfgTankCapVar_[i].size = 0;
        cfg.registerVar(cfgTankCapVar_[i], kCfgModuleId, localBranchId);

        cfgTankInitVar_[i].nvsKey = slot.tankInitKey;
        cfgTankInitVar_[i].jsonName = "tank_init_ml";
        cfgTankInitVar_[i].moduleName = slot.configModuleName;
        cfgTankInitVar_[i].type = ConfigType::Float;
        cfgTankInitVar_[i].value = &s.def.tankInitialMl;
        cfgTankInitVar_[i].persistence = ConfigPersistence::Persistent;
        cfgTankInitVar_[i].size = 0;
        cfg.registerVar(cfgTankInitVar_[i], kCfgModuleId, localBranchId);

        cfgMaxUptimeVar_[i].nvsKey = slot.maxUptimeKey;
        cfgMaxUptimeVar_[i].jsonName = "max_uptime_day_s";
        cfgMaxUptimeVar_[i].moduleName = slot.configModuleName;
        cfgMaxUptimeVar_[i].type = ConfigType::Int32;
        cfgMaxUptimeVar_[i].value = &s.def.maxUptimeDaySec;
        cfgMaxUptimeVar_[i].persistence = ConfigPersistence::Persistent;
        cfgMaxUptimeVar_[i].size = 0;
        cfg.registerVar(cfgMaxUptimeVar_[i], kCfgModuleId, localBranchId);
    }

    if (cmdSvc_ && cmdSvc_->registerHandler) {
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.override", cmdOverride_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.override_on", cmdOverrideOn_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.override_off", cmdOverrideOff_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.release", cmdOverrideRelease_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.override.duration.set", cmdOverrideDuration_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.override.select", cmdOverrideSelect_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.setpoint", cmdPoolSetpoint_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.mode", cmdPoolMode_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.write", cmdPoolWrite_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pool.refill", cmdPoolRefill_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.uptime.reset", cmdPoolResetUptime_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "pooldevice.uptime.reset_all", cmdPoolResetUptimeAll_, this);
    }

    uint8_t count = 0;
    for (uint8_t i = 0; i < POOL_DEVICE_MAX; ++i) {
        if (slots_[i].used) ++count;
    }
    BufferUsageTracker::note(TrackedBufferId::PoolDeviceSlots,
                             (size_t)count * sizeof(PoolDeviceSlot),
                             (size_t)POOL_DEVICE_MAX * sizeof(PoolDeviceSlot),
                             "init",
                             nullptr);
    LOGI("PoolDevice module ready (devices=%u)", (unsigned)count);
    (void)logHub_;
}

void PoolDeviceModule::onConfigPrepared(ConfigStore&, ServiceRegistry&)
{
    if (!slots_ || !dataStore_) return;
    const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t bytesBefore = dataStore_->values.allocatedBytes();
    const auto valuesBefore = dataStore_->values.used();
    for (uint8_t slot = 0; slot < POOL_DEVICE_MAX; ++slot) {
        auto& device = slots_[slot];
        if (!device.used || !device.def.enabled) continue;
        if (!parsePoolDriverConfig(device.driverJson, device.driverConfig, device.driverError, sizeof(device.driverError))) continue;
        if (!SerialEquipmentValues::define(*dataStore_, slot, device.def.label, device.driverConfig))
            LOGE("Serial equipment value declaration failed slot=%u", unsigned(slot));
    }
    LOGI("Serial catalog values=%u value_psram_bytes=%u internal_delta=%ld internal_largest=%u",
        unsigned(dataStore_->values.used() - valuesBefore),
        unsigned(dataStore_->values.allocatedBytes() - bytesBefore),
        long(internalBefore) - long(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

void PoolDeviceModule::onConfigLoaded(ConfigStore&, ServiceRegistry& services)
{
    static constexpr MqttConfigRouteProducer::Route kPoolDeviceCfgRoutes[] = {
        {kCfgMsgBasePdm,
         {(uint8_t)ConfigModuleId::PoolDevice, ConfigBranchRef::UnknownLocalBranch},
         nullptr,
         "",
         (uint8_t)MqttPublishPriority::Low,
         &PoolDeviceModule::buildCfgBasePdmStatic_,
         kPoolDeviceCfgTopicBase},
#define FLOW_POOLDEVICE_CFG_ROUTES(SLOT) \
        {(uint16_t)(kCfgMsgPdmSlotBase + (SLOT)), \
         {(uint8_t)ConfigModuleId::PoolDevice, poolDeviceCfgBranchFromSlot_(SLOT)}, \
         PoolDeviceSlots::kSlots[SLOT].configModuleName, \
         PoolDeviceSlots::kSlots[SLOT].id, \
         (uint8_t)MqttPublishPriority::Normal, \
         nullptr, \
         kPoolDeviceCfgTopicBase}
        FLOW_POOLDEVICE_CFG_ROUTES(0),
        FLOW_POOLDEVICE_CFG_ROUTES(1),
        FLOW_POOLDEVICE_CFG_ROUTES(2),
        FLOW_POOLDEVICE_CFG_ROUTES(3),
        FLOW_POOLDEVICE_CFG_ROUTES(4),
        FLOW_POOLDEVICE_CFG_ROUTES(5),
        FLOW_POOLDEVICE_CFG_ROUTES(6),
        FLOW_POOLDEVICE_CFG_ROUTES(7),
        FLOW_POOLDEVICE_CFG_ROUTES(8),
        FLOW_POOLDEVICE_CFG_ROUTES(9),
        FLOW_POOLDEVICE_CFG_ROUTES(10),
        FLOW_POOLDEVICE_CFG_ROUTES(11),
        FLOW_POOLDEVICE_CFG_ROUTES(12),
        FLOW_POOLDEVICE_CFG_ROUTES(13),
        FLOW_POOLDEVICE_CFG_ROUTES(14),
        FLOW_POOLDEVICE_CFG_ROUTES(15),
#undef FLOW_POOLDEVICE_CFG_ROUTES
    };
    static_assert((sizeof(kPoolDeviceCfgRoutes) / sizeof(kPoolDeviceCfgRoutes[0])) <= MqttConfigRouteProducer::MaxRoutes,
                  "PoolDevice MQTT config route table exceeds producer capacity");

    mqttSvc_ = services.get<MqttService>(ServiceId::Mqtt);
    if (!cfgMqttPub_) {
        cfgMqttPub_ = new (std::nothrow) MqttConfigRouteProducer();
    }
    if (cfgMqttPub_) {
        cfgMqttPub_->configure(this,
                               kPoolDeviceCfgProducerId,
                               kPoolDeviceCfgRoutes,
                               (uint8_t)(sizeof(kPoolDeviceCfgRoutes) / sizeof(kPoolDeviceCfgRoutes[0])),
                               services);
    }

    for (uint8_t i = 0; i < POOL_DEVICE_MAX; ++i) {
        PoolDeviceSlot& s = slots_[i];
        if (!s.used) continue;
        const bool driverParsed = parsePoolDriverConfig(s.driverJson, s.driverConfig, s.driverError, sizeof(s.driverError));
        if (driverParsed) registerDriverHa_(i);
        if (dataStore_) {
            // Follow the configured primary output, not the PoolDevice slot number.
            IoEndpointMeta output{};
            const bool hasOutputName = driverParsed &&
                s.driverConfig.capabilities.kind != PoolControlKind::Rs485 &&
                ioSvc_ && ioSvc_->meta &&
                ioSvc_->meta(ioSvc_->ctx, s.driverConfig.outputs[0], &output) == IO_OK &&
                output.name[0];
            PoolDeviceValues::setNames(dataStore_->values, i,
                                      hasOutputName ? output.name : s.def.label);
        }
        (void)loadPersistedMetrics_(i, s);
        if (runtimePersistBuf_[i][0] != '\0') {
            BufferUsageTracker::note(TrackedBufferId::PoolDeviceRuntimePersistTable,
                                     runtimePersistUsage_(),
                                     runtimePersistCapacity_(),
                                     s.id,
                                     runtimePersistBuf_[i]);
        }
    }
    restoreOverrides_();
    registerOverrideHa_();
    requestPeriodReconcile_();
}

void PoolDeviceModule::loop()
{
    if (!lockState_(pdMS_TO_TICKS(500))) {
        LOGW("PoolDevice loop skipped: state lock timeout");
        vTaskDelay(pdMS_TO_TICKS(200));
        return;
    }

    if (!runtimeReady_) {
        if (!configureRuntime_()) {
            unlockState_();
            vTaskDelay(pdMS_TO_TICKS(250));
            return;
        }
    }

    tickDevices_(millis());
    unlockState_();
    vTaskDelay(pdMS_TO_TICKS(20));
}

void PoolDeviceModule::registerDriverHa_(uint8_t slot)
{
    auto& s = slots_[slot];
    const auto& c = s.driverConfig.capabilities;
    if (!haSvc_ || !s.def.enabled || c.kind == PoolControlKind::Relay) return;
    snprintf(s.haSuffix, sizeof(s.haSuffix), "pd%u_setpoint", unsigned(slot));
    snprintf(s.haTopic, sizeof(s.haTopic), "rt/pdm/state/pd%u", unsigned(slot));
    snprintf(s.haCommand, sizeof(s.haCommand),
             "{\\\"cmd\\\":\\\"pooldevice.setpoint\\\",\\\"args\\\":{\\\"slot\\\":%u,\\\"value\\\":{{ value | float }}}}", unsigned(slot));
    if (c.kind == PoolControlKind::Discrete && haSvc_->addSelect) {
        SpiRamJsonDocument options(512);
        auto array = options.to<JsonArray>();
        for (uint8_t i = 0; i < c.stepCount; ++i) {
            char value[24]; snprintf(value, sizeof(value), "%.6g", double(c.steps[i])); array.add(value);
        }
        serializeJson(options, s.haOptions, sizeof(s.haOptions));
        const HASelectEntry entry{"pooldev", s.haSuffix, s.def.label, s.haTopic,
            "{{ '%g' | format(value_json.setpoint) }}", MqttTopics::SuffixCmd, s.haCommand, s.haOptions, "mdi:pump", nullptr};
        (void)haSvc_->addSelect(haSvc_->ctx, &entry);
    } else if (haSvc_->addNumber) {
        const HANumberEntry entry{"pooldev", s.haSuffix, s.def.label, s.haTopic,
            "{{ value_json.setpoint }}", MqttTopics::SuffixCmd, s.haCommand,
            c.minimum, c.maximum, c.unit == PoolSetpointUnit::Rpm ? 1.0f : 0.1f,
            "box", nullptr, "mdi:pump", poolSetpointUnitSymbol(c.unit)};
        (void)haSvc_->addNumber(haSvc_->ctx, &entry);
    }
    const auto& serial = s.driverConfig.serial;
    if (serial.modes.count && haSvc_->addSelect) {
        snprintf(s.haModeSuffix, sizeof(s.haModeSuffix), "pd%u_mode", unsigned(slot));
        if (buildPoolModeHaTemplates(serial.modes, slot, s.haModeOptions, sizeof(s.haModeOptions),
                                    s.haModeCommand, sizeof(s.haModeCommand))) {
            const HASelectEntry entry{"pooldev", s.haModeSuffix, s.def.label, s.haTopic,
                "{{ value_json.modes[value_json.mode] }}", MqttTopics::SuffixCmd, s.haModeCommand,
                s.haModeOptions, "mdi:heat-pump", nullptr};
            if (!haSvc_->addSelect(haSvc_->ctx, &entry)) LOGW("HA mode registration failed slot=%u", unsigned(slot));
        } else LOGW("HA mode template exceeds capacity slot=%u", unsigned(slot));
    }
    if (serial.telemetryProfile != PoolTelemetryProfile::None && haSvc_->addSensor) {
        snprintf(s.haTelemetrySuffix, sizeof(s.haTelemetrySuffix), "pd%u_diagnostics", unsigned(slot));
        const HASensorEntry entry{"pooldev", s.haTelemetrySuffix, s.def.label, s.haTopic,
            "{{ value_json.telemetry_error }}", "diagnostic", "mdi:heat-pump", nullptr, true, nullptr, false,
            "{{ value_json.telemetry | tojson }}"};
        if (!haSvc_->addSensor(haSvc_->ctx, &entry)) LOGW("HA telemetry registration failed slot=%u", unsigned(slot));
    }
}

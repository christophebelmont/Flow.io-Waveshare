#include "ModbusTcpServerModule.h"
#include "Modules/PoolDeviceModule/PoolDeviceModule.h"
#include "Modules/PoolDeviceModule/PoolDeviceModuleDataModel.h"
#include <new>

namespace {
constexpr uint8_t kModbusConfigProducerId = 91U;
constexpr MqttConfigRouteProducer::Route kModbusConfigRoutes[] = {
    {1, {(uint8_t)ConfigModuleId::ModbusTcpServer, 1U},
     "modbustcp502", "modbustcp/502", (uint8_t)MqttPublishPriority::Normal, &ModbusTcpServerModule::buildConfigMqtt_},
    {2, {(uint8_t)ConfigModuleId::ModbusTcpServer, 2U},
     "modbustcp503", "modbustcp/503", (uint8_t)MqttPublishPriority::Normal, &ModbusTcpServerModule::buildConfigMqtt_},
    {3, {(uint8_t)ConfigModuleId::ModbusTcpServer, 3U},
     "modbustcp504", "modbustcp/504", (uint8_t)MqttPublishPriority::Normal, &ModbusTcpServerModule::buildConfigMqtt_}
};

// Editable Home Assistant configuration entities on the existing HAService.
// Commands use Flow.io's authenticated MQTT broker and native cfg/set path,
// which validates and persists each field before republishing the state.
// Active TCP/RS485 listeners still apply the new settings on the next reboot.
constexpr HASelectEntry kModbusHaSelects[] = {
    {"modbustcp", "modbus_tcp_502_mode", "Modbus 502 Liaison RS485", "cfg/modbustcp/502", "{{ {0:'Aucun',1:'Equipement Flow.io',2:'Manuel'}.get(value_json.mode | int,'Aucun') }}", "cfg/set", "{{ {'modbustcp502': {'mode': {'Aucun':0,'Equipement Flow.io':1,'Manuel':2}.get(value,0)}} | to_json }}", "[\"Aucun\",\"Equipement Flow.io\",\"Manuel\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_502_parity", "Modbus 502 Parite RS485", "cfg/modbustcp/502", "{{ {0:'Aucune',1:'Paire',2:'Impaire'}.get(value_json.parity | int,'Aucune') }}", "cfg/set", "{{ {'modbustcp502': {'parity': {'Aucune':0,'Paire':1,'Impaire':2}.get(value,0)}} | to_json }}", "[\"Aucune\",\"Paire\",\"Impaire\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_502_stop_bits", "Modbus 502 Bits stop RS485", "cfg/modbustcp/502", "{{ {1:'1',2:'2'}.get(value_json.stop_bits | int,'1') }}", "cfg/set", "{{ {'modbustcp502': {'stop_bits': {'1':1,'2':2}.get(value,1)}} | to_json }}", "[\"1\",\"2\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_502_error_mode", "Modbus 502 Erreur Modbus", "cfg/modbustcp/502", "{{ {0:'Exception Modbus',1:'Silence'}.get(value_json.error_mode | int,'Exception Modbus') }}", "cfg/set", "{{ {'modbustcp502': {'error_mode': {'Exception Modbus':0,'Silence':1}.get(value,0)}} | to_json }}", "[\"Exception Modbus\",\"Silence\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_503_mode", "Modbus 503 Liaison RS485", "cfg/modbustcp/503", "{{ {0:'Aucun',1:'Equipement Flow.io',2:'Manuel'}.get(value_json.mode | int,'Aucun') }}", "cfg/set", "{{ {'modbustcp503': {'mode': {'Aucun':0,'Equipement Flow.io':1,'Manuel':2}.get(value,0)}} | to_json }}", "[\"Aucun\",\"Equipement Flow.io\",\"Manuel\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_503_parity", "Modbus 503 Parite RS485", "cfg/modbustcp/503", "{{ {0:'Aucune',1:'Paire',2:'Impaire'}.get(value_json.parity | int,'Aucune') }}", "cfg/set", "{{ {'modbustcp503': {'parity': {'Aucune':0,'Paire':1,'Impaire':2}.get(value,0)}} | to_json }}", "[\"Aucune\",\"Paire\",\"Impaire\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_503_stop_bits", "Modbus 503 Bits stop RS485", "cfg/modbustcp/503", "{{ {1:'1',2:'2'}.get(value_json.stop_bits | int,'1') }}", "cfg/set", "{{ {'modbustcp503': {'stop_bits': {'1':1,'2':2}.get(value,1)}} | to_json }}", "[\"1\",\"2\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_503_error_mode", "Modbus 503 Erreur Modbus", "cfg/modbustcp/503", "{{ {0:'Exception Modbus',1:'Silence'}.get(value_json.error_mode | int,'Exception Modbus') }}", "cfg/set", "{{ {'modbustcp503': {'error_mode': {'Exception Modbus':0,'Silence':1}.get(value,0)}} | to_json }}", "[\"Exception Modbus\",\"Silence\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_504_mode", "Modbus 504 Liaison RS485", "cfg/modbustcp/504", "{{ {0:'Aucun',1:'Equipement Flow.io',2:'Manuel'}.get(value_json.mode | int,'Aucun') }}", "cfg/set", "{{ {'modbustcp504': {'mode': {'Aucun':0,'Equipement Flow.io':1,'Manuel':2}.get(value,0)}} | to_json }}", "[\"Aucun\",\"Equipement Flow.io\",\"Manuel\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_504_parity", "Modbus 504 Parite RS485", "cfg/modbustcp/504", "{{ {0:'Aucune',1:'Paire',2:'Impaire'}.get(value_json.parity | int,'Aucune') }}", "cfg/set", "{{ {'modbustcp504': {'parity': {'Aucune':0,'Paire':1,'Impaire':2}.get(value,0)}} | to_json }}", "[\"Aucune\",\"Paire\",\"Impaire\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_504_stop_bits", "Modbus 504 Bits stop RS485", "cfg/modbustcp/504", "{{ {1:'1',2:'2'}.get(value_json.stop_bits | int,'1') }}", "cfg/set", "{{ {'modbustcp504': {'stop_bits': {'1':1,'2':2}.get(value,1)}} | to_json }}", "[\"1\",\"2\"]", "mdi:tune", "config"},
    {"modbustcp", "modbus_tcp_504_error_mode", "Modbus 504 Erreur Modbus", "cfg/modbustcp/504", "{{ {0:'Exception Modbus',1:'Silence'}.get(value_json.error_mode | int,'Exception Modbus') }}", "cfg/set", "{{ {'modbustcp504': {'error_mode': {'Exception Modbus':0,'Silence':1}.get(value,0)}} | to_json }}", "[\"Exception Modbus\",\"Silence\"]", "mdi:tune", "config"},
};
constexpr HANumberEntry kModbusHaNumbers[] = {
    {"modbustcp", "modbus_tcp_502_device_id", "Modbus 502 Equipement RS485", "cfg/modbustcp/502", "{{ value_json.device_id | int }}", "cfg/set", "{{ {'modbustcp502': {'device_id': value | int}} | to_json }}", 0, (float)(POOL_DEVICE_MAX - 1U), 1, "box", "config", "mdi:tune", nullptr},
    {"modbustcp", "modbus_tcp_502_baud", "Modbus 502 Baud RS485", "cfg/modbustcp/502", "{{ value_json.baud | int }}", "cfg/set", "{{ {'modbustcp502': {'baud': value | int}} | to_json }}", 1200, 115200, 1, "box", "config", "mdi:tune", nullptr},
    {"modbustcp", "modbus_tcp_502_timeout_ms", "Modbus 502 Timeout Modbus (ms)", "cfg/modbustcp/502", "{{ value_json.timeout_ms | int }}", "cfg/set", "{{ {'modbustcp502': {'timeout_ms': value | int}} | to_json }}", 100, 120000, 100, "box", "config", "mdi:tune", nullptr},
    {"modbustcp", "modbus_tcp_503_device_id", "Modbus 503 Equipement RS485", "cfg/modbustcp/503", "{{ value_json.device_id | int }}", "cfg/set", "{{ {'modbustcp503': {'device_id': value | int}} | to_json }}", 0, (float)(POOL_DEVICE_MAX - 1U), 1, "box", "config", "mdi:tune", nullptr},
    {"modbustcp", "modbus_tcp_503_baud", "Modbus 503 Baud RS485", "cfg/modbustcp/503", "{{ value_json.baud | int }}", "cfg/set", "{{ {'modbustcp503': {'baud': value | int}} | to_json }}", 1200, 115200, 1, "box", "config", "mdi:tune", nullptr},
    {"modbustcp", "modbus_tcp_503_timeout_ms", "Modbus 503 Timeout Modbus (ms)", "cfg/modbustcp/503", "{{ value_json.timeout_ms | int }}", "cfg/set", "{{ {'modbustcp503': {'timeout_ms': value | int}} | to_json }}", 100, 120000, 100, "box", "config", "mdi:tune", nullptr},
    {"modbustcp", "modbus_tcp_504_device_id", "Modbus 504 Equipement RS485", "cfg/modbustcp/504", "{{ value_json.device_id | int }}", "cfg/set", "{{ {'modbustcp504': {'device_id': value | int}} | to_json }}", 0, (float)(POOL_DEVICE_MAX - 1U), 1, "box", "config", "mdi:tune", nullptr},
    {"modbustcp", "modbus_tcp_504_baud", "Modbus 504 Baud RS485", "cfg/modbustcp/504", "{{ value_json.baud | int }}", "cfg/set", "{{ {'modbustcp504': {'baud': value | int}} | to_json }}", 1200, 115200, 1, "box", "config", "mdi:tune", nullptr},
    {"modbustcp", "modbus_tcp_504_timeout_ms", "Modbus 504 Timeout Modbus (ms)", "cfg/modbustcp/504", "{{ value_json.timeout_ms | int }}", "cfg/set", "{{ {'modbustcp504': {'timeout_ms': value | int}} | to_json }}", 100, 120000, 100, "box", "config", "mdi:tune", nullptr},
};
} // namespace

// Human-readable MQTT metadata is appended to the original ConfigStore JSON.
// All original numeric fields remain unchanged for backwards compatibility
// with Home Assistant templates and inbound cfg/set commands.
MqttBuildResult ModbusTcpServerModule::buildConfigMqtt_(
    void* owner, uint16_t messageId, MqttBuildContext& ctx)
{
    auto* module = static_cast<ModbusTcpServerModule*>(owner);
    if (!module || messageId < 1U || messageId > ModbusTcpServer::kPortCount ||
        !module->registry_ || !ctx.topic || !ctx.payload ||
        !ctx.topicCapacity || !ctx.payloadCapacity) return MqttBuildResult::PermanentError;

    const uint8_t portIndex = uint8_t(messageId - 1U);
    const MqttService* mqtt =
        module->registry_->get<MqttService>(ServiceId::Mqtt);
    const ConfigStoreService* cfg =
        module->registry_->get<ConfigStoreService>(ServiceId::ConfigStore);
    if (!mqtt || !mqtt->formatTopic || !cfg || !cfg->toJsonModule)
        return MqttBuildResult::RetryLater;

    char suffix[32]{};
    snprintf(suffix, sizeof(suffix), "cfg/modbustcp/%u",
             (unsigned)ModbusTcpServer::kPorts[portIndex]);
    mqtt->formatTopic(mqtt->ctx, suffix, ctx.topic, ctx.topicCapacity);
    if (ctx.topic[0] == '\0') return MqttBuildResult::PermanentError;

    bool truncated = false;
    if (!cfg->toJsonModule(cfg->ctx, kConfigModules_[portIndex],
                           ctx.payload, ctx.payloadCapacity, &truncated))
        return MqttBuildResult::NoLongerNeeded;
    if (truncated) return MqttBuildResult::PermanentError;

    const size_t baseLen = strnlen(ctx.payload, ctx.payloadCapacity);
    if (baseLen >= 2U && baseLen < ctx.payloadCapacity &&
        ctx.payload[baseLen - 1U] == '}') {
        const ConfigPort& p = module->portSettings_[portIndex];
        const char* modeLabel = p.mode == 0U ? "Aucun" :
            p.mode == 1U ? "Equipement Flow.io" : "Configuration manuelle";
        const char* errorLabel =
            p.errorMode == 1U ? "Silence" : "Exception Modbus";
        char deviceLabel[64] = "Aucun";
        uint8_t rtuAddress = 0U;
        uint32_t effectiveBaud = 0U;
        if (p.mode == 1U) {
            SerialLineProfile line{};
            char rawLabel[48]{};
            if (module->serialDeviceInfo(p.deviceId, line, rawLabel,
                                         sizeof(rawLabel), &rtuAddress)) {
                effectiveBaud = line.baud;
                // Avoid injecting quotes, backslashes or control characters
                // from user-defined device labels into the MQTT JSON object.
                size_t j = 0U;
                for (size_t i = 0U; rawLabel[i] && j + 1U < sizeof(deviceLabel); ++i) {
                    const uint8_t ch = uint8_t(rawLabel[i]);
                    deviceLabel[j++] = ch == '"' || ch == '\\' || ch < 0x20U
                        ? '_' : char(ch);
                }
                deviceLabel[j] = '\0';
            } else {
                snprintf(deviceLabel, sizeof(deviceLabel), "Equipement indisponible");
            }
        } else if (p.mode == 2U) {
            snprintf(deviceLabel, sizeof(deviceLabel), "Configuration manuelle");
            effectiveBaud = (uint32_t)p.baud;
        }

        char extension[300]{};
        const int extLen = snprintf(extension, sizeof(extension),
            ",\"mode_label\":\"%s\","
            "\"device_label\":\"%s\","
            "\"rtu_unit_id\":%u,"
            "\"serial_baud_effective\":%lu,"
            "\"error_mode_label\":\"%s\","
            "\"restart_required\":true}",
            modeLabel, deviceLabel, (unsigned)rtuAddress,
            (unsigned long)effectiveBaud, errorLabel);
        // A small MQTT payload buffer must not break the existing config or
        // HA entities: fall back to the valid original numeric JSON.
        if (extLen > 0 && size_t(extLen) < sizeof(extension) &&
            baseLen - 1U + size_t(extLen) < ctx.payloadCapacity) {
            memcpy(ctx.payload + baseLen - 1U, extension, size_t(extLen) + 1U);
        }
    }
    ctx.topicLen = (uint16_t)strnlen(ctx.topic, ctx.topicCapacity);
    ctx.payloadLen = (uint16_t)strnlen(ctx.payload, ctx.payloadCapacity);
    ctx.qos = 1U;
    ctx.retain = true;
    return MqttBuildResult::Ready;
}

// Web/bootlog snapshots: each entry fits LogEntry.msg (159 visible bytes).
// Per-port counters are cumulative. No per-frame logging on the hot path.
void ModbusTcpServerModule::logWebSnapshot_(
    uint32_t now, bool stationConnected, bool otaBusy)
{
    if (!Log::hub()) return;
    const auto& a = server_.stats(0U);
    const auto& b = server_.stats(1U);
    const auto& c = server_.stats(2U);
    const uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ModbusMasterStats queue{};
    if (master_ && master_->stats) (void)master_->stats(master_->ctx, &queue);

    // Boot-local counters: actual listener stops/reopens only, not TCP SYN
    // attempts (invisible to the application after listeners close).
    Log::info(kLogModule_,
              "[MODBUS-OTA] paused=%lu resumed=%lu pending=%u busy=%u listen=%u",
              (unsigned long)otaSuspensions_, (unsigned long)otaResumptions_,
              otaResumePending_ ? 1U : 0U, otaBusy ? 1U : 0U,
              running_ ? 1U : 0U);
    if (otaBusy) {
        Log::info(kLogModule_,
                  "[OTA-NET] ms=%lu en=%u%u%u sta=%u listen=%u clients=%u,%u,%u psram=%u",
                  (unsigned long)now,
                  configs_[0].enabled ? 1U : 0U,
                  configs_[1].enabled ? 1U : 0U,
                  configs_[2].enabled ? 1U : 0U,
                  stationConnected ? 1U : 0U, running_ ? 1U : 0U,
                  (unsigned)a.activeClients, (unsigned)b.activeClients,
                  (unsigned)c.activeClients, (unsigned)server_.psramFrameBytes());
        Log::info(kLogModule_,
                  "[OTA-NET] rtu_q=%u reject=%lu tick_us=%lu slow=%lu internal=%lu largest=%lu",
                  (unsigned)queue.queued, (unsigned long)queue.queueRejected,
                  (unsigned long)otaTickMaxUs_, (unsigned long)otaSlowTicks_,
                  (unsigned long)internal, (unsigned long)largest);
    } else {
        Log::info(kLogModule_,
                  "[MODBUS-PSRAM] bytes=%u ready=%u en=%u%u%u sta=%u listen=%u",
                  (unsigned)server_.psramFrameBytes(),
                  server_.frameStorageReady() ? 1U : 0U,
                  configs_[0].enabled ? 1U : 0U,
                  configs_[1].enabled ? 1U : 0U,
                  configs_[2].enabled ? 1U : 0U,
                  stationConnected ? 1U : 0U, running_ ? 1U : 0U);
        Log::info(kLogModule_,
                  "[MODBUS-MEM] int=%lu largest=%lu min=%lu psram=%lu stack=%lu rtu_q=%u reject=%lu",
                  (unsigned long)internal, (unsigned long)largest,
                  (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                  (unsigned long)uxTaskGetStackHighWaterMark(nullptr),
                  (unsigned)queue.queued, (unsigned long)queue.queueRejected);
    }
    for (uint8_t i = 0U; i < ModbusTcpServer::kPortCount; ++i) {
        const auto& p = server_.stats(i);
        const unsigned port = (unsigned)ModbusTcpServer::kPorts[i];
        if (otaBusy) {
            Log::info(kLogModule_,
                      "[OTA-TCP] port=%u clients=%u req=%lu reply=%lu exc=%lu tmo=%lu rxerr=%lu txerr=%lu",
                      port, (unsigned)p.activeClients,
                      (unsigned long)p.received, (unsigned long)p.completed,
                      (unsigned long)p.exceptionsSent, (unsigned long)p.timeouts,
                      (unsigned long)p.rxErrors, (unsigned long)p.txErrors);
        } else {
            Log::info(kLogModule_,
                      "[MODBUS-TCP] port=%u clients=%u peak=%u req=%lu reply=%lu exc=%lu tmo=%lu rej=%lu",
                      port, (unsigned)p.activeClients, (unsigned)p.maxActiveClients,
                      (unsigned long)p.received, (unsigned long)p.completed,
                      (unsigned long)p.exceptionsSent, (unsigned long)p.timeouts,
                      (unsigned long)p.queueRejected);
            if (p.rxErrors || p.txErrors || p.invalidTcpAdus ||
                p.resolverFailures || p.silentFailures ||
                p.connectionsRejected || p.keepaliveSetupFailures) {
                Log::warn(kLogModule_,
                          "[MODBUS-ERR] port=%u rxerr=%lu txerr=%lu invalid=%lu resolver=%lu silent=%lu connrej=%lu kaerr=%lu",
                          port, (unsigned long)p.rxErrors, (unsigned long)p.txErrors,
                          (unsigned long)p.invalidTcpAdus,
                          (unsigned long)p.resolverFailures,
                          (unsigned long)p.silentFailures,
                          (unsigned long)p.connectionsRejected,
                          (unsigned long)p.keepaliveSetupFailures);
            }
            if (p.resolverFailures) {
                Log::warn(kLogModule_,
                          "[MODBUS-RESOLVE] port=%u mode=%u device=%u unit=%u fc=%u addr=%u fails=%lu age_ms=%lu",
                          port,
                          configs_[i].serialSource == ModbusTcpServer::SerialSource::Manual ? 2U : 1U,
                          (unsigned)configs_[i].deviceId,
                          (unsigned)p.lastResolverUnitId,
                          (unsigned)p.lastResolverFunction,
                          (unsigned)p.lastResolverAddress,
                          (unsigned long)p.resolverFailures,
                          (unsigned long)(now - p.lastResolverAtMs));
            }
            if (p.received || p.completed) {
                Log::info(kLogModule_,
                          "[MODBUS-LAT] port=%u tcpmax_ms=%lu queue_ms=%lu rtu_ms=%lu",
                          port, (unsigned long)p.responseTimeMaxMs,
                          (unsigned long)p.queueWaitMaxMs,
                          (unsigned long)p.rtuResponseMaxMs);
            }
        }
    }
}

void ModbusTcpServerModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    registry_ = &services;
    master_ = services.get<ModbusMasterService>(ServiceId::ModbusMaster);
    constexpr uint8_t kModule = (uint8_t)ConfigModuleId::ModbusTcpServer;
    for (uint8_t i = 0U; i < ModbusTcpServer::kPortCount; ++i) {
        ConfigPort& p = portSettings_[i];
        const uint8_t branch = i + 1U;
        cfg.registerVar(p.modeVar, kModule, branch);
        cfg.registerVar(p.deviceVar, kModule, branch);
        cfg.registerVar(p.baudVar, kModule, branch);
        cfg.registerVar(p.parityVar, kModule, branch);
        cfg.registerVar(p.stopBitsVar, kModule, branch);
        cfg.registerVar(p.timeoutVar, kModule, branch);
        cfg.registerVar(p.errorVar, kModule, branch);
    }
    (void)cfg.registerValidator(&ModbusTcpServerModule::validateConfigCandidate_, this);
}

bool ModbusTcpServerModule::validateConfigCandidate_(
    void* context, const ConfigCandidate& candidate)
{
    const auto* self = static_cast<const ModbusTcpServerModule*>(context);
    if (!self) return false;
    for (uint8_t i = 0U; i < ModbusTcpServer::kPortCount; ++i) {
        const ConfigPort& p = self->portSettings_[i];
        uint8_t mode{}, parity{}, stop{}, error{};
        uint16_t device{};
        int32_t baud{}, timeout{};
        if (!candidate.read(p.modeVar, p.mode, mode) ||
            !candidate.read(p.deviceVar, p.deviceId, device) ||
            !candidate.read(p.baudVar, p.baud, baud) ||
            !candidate.read(p.parityVar, p.parity, parity) ||
            !candidate.read(p.stopBitsVar, p.stopBits, stop) ||
            !candidate.read(p.timeoutVar, p.timeoutMs, timeout) ||
            !candidate.read(p.errorVar, p.errorMode, error)) return false;
        if (mode > 2U || device >= POOL_DEVICE_MAX ||
            baud < 1200 || baud > 115200 || parity > 2U ||
            (stop != 1U && stop != 2U) ||
            timeout < 100 || timeout > 120000 || error > 1U) return false;
    }
    return true;
}

ModbusTcpServer::PortConfig ModbusTcpServerModule::fromSettings_(uint8_t index) const
{
    ModbusTcpServer::PortConfig config{};
    if (!ModbusTcpServer::validPortIndex(index)) return config;
    const ConfigPort& p = portSettings_[index];
    config.tcpPort = ModbusTcpServer::kPorts[index];
    config.enabled = p.mode != 0U;
    config.serialSource = p.mode == 2U
        ? ModbusTcpServer::SerialSource::Manual
        : ModbusTcpServer::SerialSource::FlowioDevice;
    config.deviceId = p.deviceId;
    config.manualLine.baud = (uint32_t)p.baud;
    config.manualLine.parity = p.parity;
    config.manualLine.stopBits = p.stopBits;
    config.transactionTimeoutMs = (uint32_t)p.timeoutMs;
    config.errorMode = p.errorMode == 1U
        ? ModbusTcpServer::ErrorMode::Silent
        : ModbusTcpServer::ErrorMode::ModbusException;
    return config;
}

void ModbusTcpServerModule::onConfigLoaded(ConfigStore& cfg, ServiceRegistry& services)
{
    for (uint8_t i = 0U; i < ModbusTcpServer::kPortCount; ++i) {
        // Import the original (single NVS blob per port) settings only once.
        // Write the mode key last as the migration completion marker, so
        // interrupted migrations can be retried without losing the old blob.
        bool migrated = false;
        if (cfg.persistentKeyExists(kConfigKeys_[i][0], migrated) && !migrated) {
            char oldKey[12]{};
            snprintf(oldKey, sizeof(oldKey), "mbtcp%u", (unsigned)i);
            StoredPort old{};
            size_t actual = 0U;
            if (cfg.readRuntimeBlob(oldKey, &old, sizeof(old), &actual) &&
                actual == sizeof(old) && old.version == kConfigVersion &&
                ModbusTcpServer::validConfig(i, old.config)) {
                const auto& saved = old.config;
                ConfigPort& p = portSettings_[i];
                const uint8_t mode = !saved.enabled ? 0U :
                    saved.serialSource == ModbusTcpServer::SerialSource::Manual ? 2U : 1U;
                const uint16_t device = saved.deviceId;
                const int32_t baud = (int32_t)saved.manualLine.baud;
                const uint8_t parity = saved.manualLine.parity;
                const uint8_t stop = saved.manualLine.stopBits;
                const int32_t timeout = (int32_t)saved.transactionTimeoutMs;
                const uint8_t error = saved.errorMode == ModbusTcpServer::ErrorMode::Silent ? 1U : 0U;
                // Copy only configurations that meet all new ConfigStore bounds.
                if (device < POOL_DEVICE_MAX && baud >= 1200 && baud <= 115200 &&
                    parity <= 2U && (stop == 1U || stop == 2U) &&
                    timeout >= 100 && timeout <= 120000) {
                    const bool ok =
                        cfg.set(p.deviceVar, device) &&
                        cfg.set(p.baudVar, baud) &&
                        cfg.set(p.parityVar, parity) &&
                        cfg.set(p.stopBitsVar, stop) &&
                        cfg.set(p.timeoutVar, timeout) &&
                        cfg.set(p.errorVar, error);
                    if (ok) {
                        // Persist every field BEFORE the migration marker.
                        // A partial or interrupted migration must be retried,
                        // not silently treated as complete on next boot.
                        const bool fieldsSaved =
                            cfg.persistRegisteredValue(&p.deviceId) &&
                            cfg.persistRegisteredValue(&p.baud) &&
                            cfg.persistRegisteredValue(&p.parity) &&
                            cfg.persistRegisteredValue(&p.stopBits) &&
                            cfg.persistRegisteredValue(&p.timeoutMs) &&
                            cfg.persistRegisteredValue(&p.errorMode);
                        if (fieldsSaved && cfg.set(p.modeVar, mode))
                            (void)cfg.persistRegisteredValue(&p.mode);
                    }
                }
            }
        }
        const auto config = fromSettings_(i);
        if (ModbusTcpServer::validConfig(i, config)) (void)configurePort(i, config);
    }

    if (!cfgMqttPub_) cfgMqttPub_ = new (std::nothrow) MqttConfigRouteProducer();
    if (cfgMqttPub_) {
        cfgMqttPub_->configure(this, kModbusConfigProducerId,
            kModbusConfigRoutes,
            (uint8_t)(sizeof(kModbusConfigRoutes) / sizeof(kModbusConfigRoutes[0])),
            services);
    }

    const HAService* ha = services.get<HAService>(ServiceId::Ha);
    if (ha) {
        if (ha->addSelect)
            for (const auto& entry : kModbusHaSelects)
                (void)ha->addSelect(ha->ctx, &entry);
        if (ha->addNumber)
            for (const auto& entry : kModbusHaNumbers)
                (void)ha->addNumber(ha->ctx, &entry);
    }
}

void ModbusTcpServerModule::setPoolDeviceModule(PoolDeviceModule* devices)
{
    devices_ = devices;
    resolver_.setDeviceLookup(&ModbusTcpServerModule::lookupDevice_, this);
}

bool ModbusTcpServerModule::lookupDevice_(void* context, uint16_t deviceId,
                                          SerialLineProfile& out, uint8_t& slaveAddress)
{
    const auto* module = static_cast<const ModbusTcpServerModule*>(context);
    return module && module->devices_ &&
        module->devices_->serialDeviceInfo(deviceId, out, nullptr, 0U, &slaveAddress);
}

bool ModbusTcpServerModule::readPort(uint8_t index, ModbusTcpServer::PortConfig& out) const
{
    if (!ModbusTcpServer::validPortIndex(index)) return false;
    out = fromSettings_(index);
    return ModbusTcpServer::validConfig(index, out);
}

bool ModbusTcpServerModule::serialDeviceInfo(uint16_t id, SerialLineProfile& line,
                                             char* label, size_t labelCapacity,
                                             uint8_t* slaveAddress) const
{
    return devices_ && devices_->serialDeviceInfo(id, line, label, labelCapacity, slaveAddress);
}

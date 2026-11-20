#pragma once
/**
 * @brief Flow.io lifecycle adapter for three Modbus TCP listeners.
 * Disabled by default until ports have valid explicit configuration.
 */
#include "Core/Module.h"
#include <esp_heap_caps.h>
#include "Core/Services/IModbusMaster.h"
#include "Core/Services/INetworkAccess.h"
#include "Core/Services/IHA.h"
#include "Modules/Network/MQTTModule/MqttConfigRouteProducer.h"
#include "Modules/Network/ModbusTcpServerModule/ModbusTcpLineResolver.h"
#include "Modules/Network/ModbusTcpServerModule/ModbusTcpSocketServer.h"

class PoolDeviceModule;

class ModbusTcpServerModule : public Module {
public:
    ModuleId moduleId() const override { return ModuleId::ModbusTcpServer; }
    const char* taskName() const override { return "modbustcp"; }
    uint8_t dependencyCount() const override { return 5; }
    ModuleId dependency(uint8_t i) const override {
        if (i == 0) return ModuleId::Io;
        if (i == 1) return ModuleId::PoolDevice;
        if (i == 2) return ModuleId::Wifi;
        if (i == 3) return ModuleId::Mqtt;
        if (i == 4) return ModuleId::Ha;
        return ModuleId::Unknown;
    }
    uint8_t taskCount() const override { return 1; }
    const ModuleTaskSpec* taskSpecs() const override { return singleLoopTaskSpec(); }
    uint16_t taskStackSize() const override { return 4096; }
    void init(ConfigStore& cfg, ServiceRegistry& services) override;
    void onConfigLoaded(ConfigStore& cfg, ServiceRegistry&) override;
    void onStart(ConfigStore&, ServiceRegistry&) override {
        // Wait for an actual station/Ethernet IP before binding sockets.
        // Never expose the unauthenticated Modbus protocol on provisioning AP.
        eligible_ = master_ && master_->submit && master_->poll;
    }
    void loop() override {
        if (!eligible_) return;
        const NetworkAccessService* network =
            registry_ ? registry_->get<NetworkAccessService>(ServiceId::NetworkAccess) : nullptr;
        const bool stationConnected = network && network->mode &&
            network->mode(network->ctx) == NetworkAccessMode::Station;
        if (!stationConnected) {
            if (running_) {
                server_.stop();
                running_ = false;
            }
            return;
        }
        if (!running_) {
            bool enabled = false;
            for (uint8_t i = 0; i < ModbusTcpServer::kPortCount; ++i)
                enabled = enabled || configs_[i].enabled;
            if (!enabled) return;
            server_.begin(*master_, &ModbusTcpServer::LineResolver::resolveStatic, &resolver_);
            running_ = true;
        }
        const uint32_t now = millis();
        const uint32_t tickStartedUs = micros();
        server_.tick(now);
        const uint32_t elapsedUs = uint32_t(micros() - tickStartedUs);
        if (elapsedUs > tickMaxUs_) tickMaxUs_ = elapsedUs;
        if (elapsedUs >= 5000U) ++slowTicks_;
        if (uint32_t(now - lastDiagnosticsMs_) < 60000U) return;
        lastDiagnosticsMs_ = now;
        ModbusMasterStats queue{};
        if (master_ && master_->stats) (void)master_->stats(master_->ctx, &queue);
        const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const uint32_t minInternal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        const uint32_t fragEstimate = freeInternal ?
            100U - (largestInternal * 100U / freeInternal) : 0U;
        const uint32_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const uint32_t largestPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        const uint32_t stackWatermark = uxTaskGetStackHighWaterMark(nullptr);
        Serial.printf("[MODBUS-TCP] tick_max_us=%lu tick_slow_5ms=%lu\n",
            (unsigned long)tickMaxUs_, (unsigned long)slowTicks_);
        tickMaxUs_ = 0U;
        slowTicks_ = 0U;
        Serial.printf("[MODBUS-TCP] heap_internal=%lu min_internal=%lu largest_internal=%lu frag_est_percent=%lu psram=%lu largest_psram=%lu stack_hwm=%lu rtu_queue=%u rtu_peak=%u rtu_rejected=%lu rtu_crc=%lu rtu_timeouts=%lu\n",
            (unsigned long)freeInternal, (unsigned long)minInternal,
            (unsigned long)largestInternal, (unsigned long)fragEstimate,
            (unsigned long)freePsram, (unsigned long)largestPsram,
            (unsigned long)stackWatermark, (unsigned)queue.queued,
            (unsigned)queue.queuedPeak, (unsigned long)queue.queueRejected,
            (unsigned long)queue.crcErrors, (unsigned long)queue.timeouts);
        for (uint8_t i = 0; i < ModbusTcpServer::kPortCount; ++i) {
            const auto& p = server_.stats(i);
            Serial.printf("[MODBUS-TCP] port=%u clients=%u peak_clients=%u requests=%lu replies=%lu exceptions=%lu silent=%lu crc=%lu timeouts=%lu rejected=%lu idle_reclaimed=%lu peer_closed=%lu ka_setup_errors=%lu resolver_fail=%lu rx_bytes=%lu rx_errors=%lu tx_bytes=%lu tx_retry=%lu tx_errors=%lu tcp_max_ms=%lu rtu_queue_max_ms=%lu rtu_response_max_ms=%lu\n",
                (unsigned)ModbusTcpServer::kPorts[i], (unsigned)p.activeClients,
                (unsigned)p.maxActiveClients, (unsigned long)p.received,
                (unsigned long)p.completed, (unsigned long)p.exceptionsSent,
                (unsigned long)p.silentFailures, (unsigned long)p.crcErrors,
                (unsigned long)p.timeouts, (unsigned long)p.connectionsRejected,
                (unsigned long)p.idleReclaimed, (unsigned long)p.peerClosed,
                (unsigned long)p.keepaliveSetupFailures, (unsigned long)p.resolverFailures,
                (unsigned long)p.tcpBytesReceived, (unsigned long)p.rxErrors,
                (unsigned long)p.tcpBytesSent, (unsigned long)p.txWouldBlock, (unsigned long)p.txErrors,
                (unsigned long)p.responseTimeMaxMs, (unsigned long)p.queueWaitMaxMs,
                (unsigned long)p.rtuResponseMaxMs);
        }
    }
    // MQTT JSON extension retains the existing numeric settings used by HA.
    // Called by the standard MqttConfigRouteProducer (no second publisher).
    static MqttBuildResult buildConfigMqtt_(void* owner, uint16_t messageId,
                                           MqttBuildContext& ctx);
    void setPoolDeviceModule(PoolDeviceModule* devices);
    /** Read ConfigStore settings (applied to listeners after a restart). */
    bool readPort(uint8_t index, ModbusTcpServer::PortConfig& out) const;
    /** Listener settings currently applied (may differ from staged ConfigStore values). */
    bool readActivePort(uint8_t index, ModbusTcpServer::PortConfig& out) const {
        if (!ModbusTcpServer::validPortIndex(index)) return false;
        out = configs_[index];
        return ModbusTcpServer::validConfig(index, out);
    }
    /** Get a serial device selectable in the web interface. */
    bool serialDeviceInfo(uint16_t id, SerialLineProfile& line,
                          char* label, size_t labelCapacity,
                          uint8_t* slaveAddress = nullptr) const;
    bool running() const { return running_; }
    bool schedulerStats(ModbusMasterStats& out) const {
        return master_ && master_->stats && master_->stats(master_->ctx, &out);
    }
    bool configurePort(uint8_t index, const ModbusTcpServer::PortConfig& config) {
        if (running_ || !ModbusTcpServer::validConfig(index, config)) return false;
        configs_[index] = config;
        resolver_.configure(index, config);
        server_.configure(index, config);
        return true;
    }
    const ModbusTcpServer::PortStats& portStats(uint8_t index) const {
        return server_.stats(index);
    }
private:
    // Each TCP port is one native ConfigStore module. Keys remain under the
    // 15-character NVS limit; the old mbtcp0..2 blobs are migration-only.
    static constexpr uint32_t kConfigVersion = 1U;
    static constexpr const char* kConfigModules_[3] = {
        "modbustcp502", "modbustcp503", "modbustcp504"
    };
    static constexpr const char* kConfigKeys_[3][7] = {
        {"mt0mode","mt0dev","mt0baud","mt0par","mt0stop","mt0tmo","mt0err"},
        {"mt1mode","mt1dev","mt1baud","mt1par","mt1stop","mt1tmo","mt1err"},
        {"mt2mode","mt2dev","mt2baud","mt2par","mt2stop","mt2tmo","mt2err"}
    };
    struct StoredPort {
        uint32_t version = 0U;
        ModbusTcpServer::PortConfig config{};
    };
    struct ConfigPort {
        uint8_t mode = 0; // 0=None; 1=Flow.io equipment; 2=manual RTU
        uint16_t deviceId = 0;
        int32_t baud = 9600;
        uint8_t parity = 0;
        uint8_t stopBits = 1;
        int32_t timeoutMs = 3000;
        uint8_t errorMode = 0;

        ConfigVariable<uint8_t,0> modeVar;
        ConfigVariable<uint16_t,0> deviceVar;
        ConfigVariable<int32_t,0> baudVar;
        ConfigVariable<uint8_t,0> parityVar;
        ConfigVariable<uint8_t,0> stopBitsVar;
        ConfigVariable<int32_t,0> timeoutVar;
        ConfigVariable<uint8_t,0> errorVar;

        ConfigPort(const char* module, const char* const keys[7])
            : modeVar{keys[0],"mode",module,ConfigType::UInt8,&mode,ConfigPersistence::Persistent,0},
              deviceVar{keys[1],"device_id",module,ConfigType::UInt16,&deviceId,ConfigPersistence::Persistent,0},
              baudVar{keys[2],"baud",module,ConfigType::Int32,&baud,ConfigPersistence::Persistent,0},
              parityVar{keys[3],"parity",module,ConfigType::UInt8,&parity,ConfigPersistence::Persistent,0},
              stopBitsVar{keys[4],"stop_bits",module,ConfigType::UInt8,&stopBits,ConfigPersistence::Persistent,0},
              timeoutVar{keys[5],"timeout_ms",module,ConfigType::Int32,&timeoutMs,ConfigPersistence::Persistent,0},
              errorVar{keys[6],"error_mode",module,ConfigType::UInt8,&errorMode,ConfigPersistence::Persistent,0} {}
    };
    ConfigPort portSettings_[3] = {
        {kConfigModules_[0], kConfigKeys_[0]},
        {kConfigModules_[1], kConfigKeys_[1]},
        {kConfigModules_[2], kConfigKeys_[2]}
    };
    static bool validateConfigCandidate_(void* context, const ConfigCandidate& candidate);
    ModbusTcpServer::PortConfig fromSettings_(uint8_t index) const;
    MqttConfigRouteProducer* cfgMqttPub_ = nullptr;
    static bool lookupDevice_(void* context, uint16_t deviceId,
                              SerialLineProfile& out, uint8_t& slaveAddress);
    PoolDeviceModule* devices_ = nullptr;
    ServiceRegistry* registry_ = nullptr;
    const ModbusMasterService* master_ = nullptr;
    ModbusTcpServer::LineResolver resolver_{};
    ModbusTcpServer::SocketServer server_{};
    ModbusTcpServer::PortConfig configs_[ModbusTcpServer::kPortCount]{};
    bool running_ = false;
    bool eligible_ = false;
    uint32_t lastDiagnosticsMs_ = 0U;
    uint32_t tickMaxUs_ = 0U;
    uint32_t slowTicks_ = 0U;
};

#pragma once
/**
 * @file WifiProvisioningModule.h
 * @brief WiFi provisioning overlay (STA fallback to AP portal).
 */

#include "Core/Module.h"
#include "Core/EventBus/EventBus.h"
#include "Core/ServiceBinding.h"
#include "Core/Services/Services.h"

#include <DNSServer.h>
#include <WiFi.h>

#ifndef ETH_TIMEOUT_MS
#define ETH_TIMEOUT_MS 7000UL
#endif

#ifndef WIFI_TIMEOUT_MS
#define WIFI_TIMEOUT_MS 12000UL
#endif

enum class NetworkPortalReason : uint8_t {
    None = 0,
    MissingCredentials = 1,
    ConnectTimeout = 2
};

namespace FlowNetwork {

class NetworkManager {
public:
    void begin(uint32_t bootMs) { bootMs_ = bootMs; }
    void updateConfig(bool ethernetEnabled, bool wifiEnabled, bool wifiConfigured) {
        ethernetEnabled_ = ethernetEnabled;
        wifiEnabled_ = wifiEnabled;
        wifiConfigured_ = wifiConfigured;
    }
    void updateInterfaces(bool ethHasIP, bool wifiHasIP) {
        const bool hadNetworkBefore = hasNetwork();
        ethHasIP_ = ethHasIP;
        wifiHasIP_ = wifiHasIP;
        const bool hasNetworkNow = hasNetwork();
        if (hasNetworkNow) {
            hadUsableNetwork_ = true;
            lastNetworkLostMs_ = 0;
        } else if (hadNetworkBefore) {
            lastNetworkLostMs_ = millis();
        }
    }
    void setCaptivePortalRunning(bool running) { captivePortalRunning_ = running; }
    void setNormalServicesStarted(bool started) { normalServicesStarted_ = started; }

    bool hasNetwork() const { return ethHasIP_ || wifiHasIP_; }
    bool hasEthernetIP() const { return ethHasIP_; }
    bool hasWifiIP() const { return wifiHasIP_; }
    bool isCaptivePortalRunning() const { return captivePortalRunning_; }
    bool normalServicesStarted() const { return normalServicesStarted_; }

    NetworkPortalReason portalReason(uint32_t nowMs) const {
        if (hasNetwork()) return NetworkPortalReason::None;

        if (hadUsableNetwork_ && lastNetworkLostMs_ != 0U &&
            (nowMs - lastNetworkLostMs_) < (uint32_t)ETH_TIMEOUT_MS) {
            return NetworkPortalReason::None;
        }

        const uint32_t ethWaitMs = ethernetEnabled_ ? (uint32_t)ETH_TIMEOUT_MS : 0U;
        const uint32_t elapsedMs = nowMs - bootMs_;
        if (elapsedMs < ethWaitMs) return NetworkPortalReason::None;

        if (!wifiEnabled_ || !wifiConfigured_) {
            return NetworkPortalReason::MissingCredentials;
        }

        if (elapsedMs < (ethWaitMs + (uint32_t)WIFI_TIMEOUT_MS)) {
            return NetworkPortalReason::None;
        }
        return NetworkPortalReason::ConnectTimeout;
    }

private:
    volatile bool ethHasIP_ = false;
    volatile bool wifiHasIP_ = false;
    bool captivePortalRunning_ = false;
    bool normalServicesStarted_ = false;
    bool hadUsableNetwork_ = false;
    bool ethernetEnabled_ = false;
    bool wifiEnabled_ = true;
    bool wifiConfigured_ = false;
    uint32_t bootMs_ = 0;
    uint32_t lastNetworkLostMs_ = 0;
};

}  // namespace FlowNetwork

class WifiProvisioningModule : public Module {
public:
    ModuleId moduleId() const override { return ModuleId::WifiProvisioning; }
    const char* taskName() const override { return "wifiprov"; }
    BaseType_t taskCore() const override { return 0; }
    uint16_t taskStackSize() const override {
        return 4096;
    }
    uint8_t taskCount() const override { return 1; }
    const ModuleTaskSpec* taskSpecs() const override { return singleLoopTaskSpec(); }

    uint8_t dependencyCount() const override { return 2; }
    ModuleId dependency(uint8_t i) const override {
        if (i == 0) return ModuleId::Wifi;
        if (i == 1) return ModuleId::LogHub;
        return ModuleId::Unknown;
    }

    void init(ConfigStore& cfg, ServiceRegistry& services) override;
    void onConfigLoaded(ConfigStore& cfg, ServiceRegistry& services) override;
    void onStart(ConfigStore& cfg, ServiceRegistry& services) override;
    void loop() override;
    uint32_t startDelayMs() const override {
        return fastPortalStart_ ? 0U : Limits::Boot::WifiProvisioningStartDelayMs;
    }

private:
    static constexpr uint32_t kConfigPollMs = 1000U;
    static constexpr uint16_t kDnsPort = 53;
    static constexpr uint32_t kApClientPollMs = 1000U;
    static constexpr uint32_t kApClientGraceMs = 120000U;
    static constexpr uint32_t kStaProbeIntervalMs = 30000U;
    static constexpr uint32_t kStaProbeWindowMs = 6000U;
    static constexpr uint32_t kApStartLogIntervalMs = 5000U;
    static constexpr uint32_t kApStartRetryMs = 2000U;
    static constexpr uint32_t kApStartMinLargestInternalBytes = 24576U;
    static constexpr uint8_t kApStartForceAfterDefers = 4U;
    static constexpr uint32_t kStaStopWaitMs = 700U;
    static constexpr uint32_t kModeResetSettleMs = 120U;
    static constexpr uint32_t kApStartStableDelayMs = 450U;
    static constexpr uint32_t kHmiPortalResyncMs = 2000U;

    ConfigStore* cfgStore_ = nullptr;
    ServiceRegistry* services_ = nullptr;
    const WifiService* wifiSvc_ = nullptr;
    const NetworkAccessService* observedNetAccessSvc_ = nullptr;
    const HmiService* hmiSvc_ = nullptr;
    EventBus* eventBus_ = nullptr;
    FlowNetwork::NetworkManager networkManager_{};

    DNSServer dns_;
    bool apActive_ = false;
    bool wifiConfigured_ = false;
    bool wifiEnabled_ = true;
    bool ethernetEnabled_ = false;
    uint8_t wifiSsidLen_ = 0;
    uint8_t wifiPassLen_ = 0;
    bool configDirty_ = false;
    bool fastPortalStart_ = false;
    bool portalLatched_ = false;
    bool hmiCaptivePortalActive_ = false;
    bool hmiCaptivePortalConditionSynced_ = false;
    bool staProbeActive_ = false;
    bool apStarting_ = false;
    volatile bool shutdownPending_ = false;
    volatile bool apRestartPending_ = false;
    volatile bool apStopEventPending_ = false;
    volatile bool apStopDuringStartEventPending_ = false;
    volatile bool apStartDuringStartEventPending_ = false;
    volatile bool apClientRefreshPending_ = false;
    volatile bool apClientConnectedEventPending_ = false;
    volatile bool apClientDisconnectedEventPending_ = false;
    volatile uint8_t apClientDisconnectReason_ = 0;
    volatile uint32_t apProbeEventCount_ = 0;
    volatile int apProbeLastRssi_ = 0;
    bool apClientEverSeen_ = false;
    uint8_t apClientCount_ = 0;
    uint32_t lastApClientSeenMs_ = 0;
    uint32_t lastApClientPollMs_ = 0;
    uint32_t lastStaProbeStartMs_ = 0;
    uint32_t bootMs_ = 0;
    uint32_t lastCfgPollMs_ = 0;
    uint32_t lastApStartPrecheckLogMs_ = 0;
    uint32_t lastApStartDeferredLogMs_ = 0;
    uint32_t lastApProbeLogMs_ = 0;
    uint32_t lastHmiCaptivePortalSyncMs_ = 0;
    uint32_t nextApStartAttemptMs_ = 0;
    uint32_t apProbeCount_ = 0;
    uint8_t apStartDeferredCount_ = 0;
    wifi_event_id_t wifiEventHandlerId_ = 0;
    char apSsid_[40] = {0};
    char apPass_[32] = {0};

    bool isWebReachable_() const;
    NetworkAccessMode mode_() const;
    bool getIP_(char* out, size_t len) const;
    bool notifyWifiConfigChanged_();
    bool notifyShutdownPending_();

    void buildApCredentials_();
    void handleStaProbePolicy_(uint32_t nowMs);
    void refreshApClientState_(uint32_t nowMs, bool fromEvent);
    void startStaProbe_(uint32_t nowMs);
    void stopStaProbe_(const char* reason);
    void refreshWifiConfig_();
    NetworkPortalReason evaluatePortalReason_() const;
    void ensurePortalStarted_();
    static void onWifiEventSys_(arduino_event_t* event);
    void onWifiEvent_(arduino_event_t* event);
    static void onEventStatic_(const Event& e, void* user);
    void onEvent_(const Event& e);
    bool startCaptivePortal_(NetworkPortalReason reason);
    void stopCaptivePortal_(const char* reason);
    bool isStaConnected_() const;
    bool hasStationNetwork_() const;
    void syncNetworkManagerState_();
    void setHmiCaptivePortalCondition_(bool active, bool force = false);
    bool getStaIp_(char* out, size_t len) const;
    bool getApIp_(char* out, size_t len) const;

    NetworkAccessService netAccessSvc_{
        ServiceBinding::bind<&WifiProvisioningModule::isWebReachable_>,
        ServiceBinding::bind<&WifiProvisioningModule::mode_>,
        ServiceBinding::bind<&WifiProvisioningModule::getIP_>,
        ServiceBinding::bind<&WifiProvisioningModule::notifyWifiConfigChanged_>,
        ServiceBinding::bind<&WifiProvisioningModule::notifyShutdownPending_>,
        this
    };
};

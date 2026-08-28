/**
 * @file WifiProvisioningModule.cpp
 * @brief WiFi provisioning overlay implementation.
 */

#include "WifiProvisioningModule.h"

#include "App/BuildFlags.h"
#include "Core/FirmwareVersion.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::WifiProvisioningModule)
#include "Core/ModuleLog.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <string.h>
#include <strings.h>

namespace {
constexpr const char* kDefaultApPass = "flowio1234";
WifiProvisioningModule* gWifiProvisioningInstance = nullptr;
constexpr wifi_auth_mode_t kProvisioningApAuthMode = WIFI_AUTH_WPA2_PSK;
constexpr wifi_cipher_type_t kProvisioningApCipher = WIFI_CIPHER_TYPE_CCMP;
constexpr uint8_t kProvisioningApChannel = 1U;
constexpr uint8_t kProvisioningApMaxConnections = 2U;

const char* wifiModeName_(wifi_mode_t mode)
{
    switch (mode) {
    case WIFI_MODE_NULL: return "NULL";
    case WIFI_MODE_STA: return "STA";
    case WIFI_MODE_AP: return "AP";
    case WIFI_MODE_APSTA: return "APSTA";
    default: return "?";
    }
}

const char* wifiAuthName_(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
#if defined(WIFI_AUTH_WPA2_ENTERPRISE)
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
#endif
#if defined(WIFI_AUTH_WPA3_PSK)
    case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
#endif
#if defined(WIFI_AUTH_WPA2_WPA3_PSK)
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
#endif
#if defined(WIFI_AUTH_WAPI_PSK)
    case WIFI_AUTH_WAPI_PSK: return "WAPI_PSK";
#endif
    default: return "?";
    }
}

const char* wifiCipherName_(wifi_cipher_type_t cipher)
{
    switch (cipher) {
    case WIFI_CIPHER_TYPE_NONE: return "NONE";
    case WIFI_CIPHER_TYPE_WEP40: return "WEP40";
    case WIFI_CIPHER_TYPE_WEP104: return "WEP104";
    case WIFI_CIPHER_TYPE_TKIP: return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP: return "CCMP";
#if defined(WIFI_CIPHER_TYPE_TKIP_CCMP)
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP_CCMP";
#endif
#if defined(WIFI_CIPHER_TYPE_AES_CMAC128)
    case WIFI_CIPHER_TYPE_AES_CMAC128: return "AES_CMAC128";
#endif
    default: return "?";
    }
}
}

void WifiProvisioningModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    cfgStore_ = &cfg;
    services_ = &services;
    wifiSvc_ = services.get<WifiService>(ServiceId::Wifi);
    hmiSvc_ = services.get<HmiService>(ServiceId::Hmi);
    const EventBusService* eventBusSvc = services.get<EventBusService>(ServiceId::EventBus);
    eventBus_ = eventBusSvc ? eventBusSvc->bus : nullptr;
    if (eventBus_) {
        eventBus_->subscribe(EventId::NetworkShutdownPending, &WifiProvisioningModule::onEventStatic_, this);
    }
    bootMs_ = millis();
    networkManager_.begin(bootMs_);
    lastCfgPollMs_ = 0;
    buildApCredentials_();

    gWifiProvisioningInstance = this;
    if (wifiEventHandlerId_ != 0U) {
        WiFi.removeEvent(wifiEventHandlerId_);
        wifiEventHandlerId_ = 0U;
    }
    wifiEventHandlerId_ = WiFi.onEvent(WifiProvisioningModule::onWifiEventSys_);

    LOGI("[NET] Provisioning overlay initialized (eth_timeout=%lu ms, wifi_timeout=%lu ms, AP SSID=%s)",
         (unsigned long)ETH_TIMEOUT_MS,
         (unsigned long)WIFI_TIMEOUT_MS,
         apSsid_);
}

void WifiProvisioningModule::onConfigLoaded(ConfigStore&, ServiceRegistry& services)
{
    services_ = &services;
    hmiSvc_ = services.get<HmiService>(ServiceId::Hmi);
    refreshWifiConfig_();
    observedNetAccessSvc_ = services.get<NetworkAccessService>(ServiceId::NetworkAccess);
    if (!ethernetEnabled_ && !services.has(ServiceId::NetworkAccess)) {
        if (!services.add(ServiceId::NetworkAccess, &netAccessSvc_)) {
            LOGE("service registration failed: %s", toString(ServiceId::NetworkAccess));
        }
        observedNetAccessSvc_ = services.get<NetworkAccessService>(ServiceId::NetworkAccess);
    }
    syncNetworkManagerState_();
    LOGI("[NET] Provisioning config loaded: ethernet=%d wifi_enabled=%d wifi_configured=%d ssid_len=%u pass_len=%u eth_timeout_ms=%lu wifi_timeout_ms=%lu",
         (int)ethernetEnabled_,
         (int)wifiEnabled_,
         (int)wifiConfigured_,
         (unsigned)wifiSsidLen_,
         (unsigned)wifiPassLen_,
         (unsigned long)ETH_TIMEOUT_MS,
         (unsigned long)WIFI_TIMEOUT_MS);
    ensurePortalStarted_();
}

void WifiProvisioningModule::onStart(ConfigStore&, ServiceRegistry&)
{
}

void WifiProvisioningModule::loop()
{
    uint32_t now = millis();
    if (apStartDuringStartEventPending_) {
        apStartDuringStartEventPending_ = false;
        LOGD("Provisioning AP start event during setup mode=%s", wifiModeName_(WiFi.getMode()));
    }
    if (apStopDuringStartEventPending_) {
        apStopDuringStartEventPending_ = false;
        LOGD("Provisioning AP stop event during setup mode=%s", wifiModeName_(WiFi.getMode()));
    }
    if (apProbeEventCount_ != 0U) {
        const uint32_t probeCount = apProbeEventCount_;
        const int probeRssi = apProbeLastRssi_;
        apProbeEventCount_ = 0;
        apProbeCount_ += probeCount;
        if ((now - lastApProbeLogMs_) >= 2000U) {
            lastApProbeLogMs_ = now;
            LOGI("AP probe activity probes=%lu clients=%u rssi=%d",
                 (unsigned long)apProbeCount_,
                 (unsigned)WiFi.softAPgetStationNum(),
                 probeRssi);
        }
    }
    if (apClientConnectedEventPending_) {
        apClientConnectedEventPending_ = false;
        LOGI("AP client connected count=%u", (unsigned)WiFi.softAPgetStationNum());
    }
    if (apClientDisconnectedEventPending_) {
        const uint8_t reason = apClientDisconnectReason_;
        apClientDisconnectedEventPending_ = false;
        const char* reasonName = WiFi.disconnectReasonName((wifi_err_reason_t)reason);
        LOGW("AP client disconnected reason=%u(%s) count=%u",
             (unsigned)reason,
             reasonName ? reasonName : "?",
             (unsigned)WiFi.softAPgetStationNum());
    }
    if (apClientRefreshPending_) {
        apClientRefreshPending_ = false;
        refreshApClientState_(now, true);
    }
    if (apStopEventPending_) {
        apStopEventPending_ = false;
        if (shutdownPending_) {
            LOGI("Provisioning AP stop observed during shutdown; restart suppressed");
        } else if (apActive_) {
            LOGE("Provisioning AP stopped unexpectedly; scheduling restart mode=%s wl=%d",
                 wifiModeName_(WiFi.getMode()),
                 (int)WiFi.status());
            apRestartPending_ = true;
        }
    }
    if (apRestartPending_) {
        apRestartPending_ = false;
        dns_.stop();
        apActive_ = false;
        networkManager_.setCaptivePortalRunning(false);
        setHmiCaptivePortalCondition_(false);
        portalLatched_ = false;
        apClientCount_ = 0;
        nextApStartAttemptMs_ = millis() + kApStartRetryMs;
    }
    if (configDirty_ || (now - lastCfgPollMs_) >= kConfigPollMs) {
        lastCfgPollMs_ = now;
        configDirty_ = false;
        refreshWifiConfig_();
    }
    syncNetworkManagerState_();
    const bool forceHmiPortalSync = apActive_ &&
        ((uint32_t)(now - lastHmiCaptivePortalSyncMs_) >= kHmiPortalResyncMs);
    setHmiCaptivePortalCondition_(apActive_, forceHmiPortalSync);

    if (hasStationNetwork_()) {
        if (apActive_) {
            stopCaptivePortal_(networkManager_.hasEthernetIP()
                                   ? "Ethernet is available"
                                   : "STA connected");
        }
        networkManager_.setNormalServicesStarted(true);
        portalLatched_ = false;
        vTaskDelay(pdMS_TO_TICKS(250));
        return;
    }

    ensurePortalStarted_();

    if (apActive_) {
        // Refresh timestamp after potential AP start delays to avoid stale-now
        // wraparound in probe interval checks.
        now = millis();
        handleStaProbePolicy_(now);
        dns_.processNextRequest();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
}

void WifiProvisioningModule::ensurePortalStarted_()
{
    if (apActive_ || portalLatched_) return;
    syncNetworkManagerState_();
    if (networkManager_.hasNetwork()) return;

    const NetworkPortalReason reason = evaluatePortalReason_();
    if (reason == NetworkPortalReason::None) return;

    if (startCaptivePortal_(reason)) {
        portalLatched_ = true;
    }
}

bool WifiProvisioningModule::isWebReachable_() const
{
    return isStaConnected_() || apActive_;
}

NetworkAccessMode WifiProvisioningModule::mode_() const
{
    if (isStaConnected_()) return NetworkAccessMode::Station;
    if (apActive_) return NetworkAccessMode::AccessPoint;
    return NetworkAccessMode::None;
}

bool WifiProvisioningModule::getIP_(char* out, size_t len) const
{
    if (!out || len == 0) return false;
    if (isStaConnected_()) {
        return getStaIp_(out, len);
    }
    if (apActive_) {
        return getApIp_(out, len);
    }
    out[0] = '\0';
    return false;
}

bool WifiProvisioningModule::notifyWifiConfigChanged_()
{
    if (shutdownPending_) return true;
    configDirty_ = true;
    if (wifiSvc_ && wifiSvc_->setStaRetryEnabled) {
        (void)wifiSvc_->setStaRetryEnabled(wifiSvc_->ctx, true);
    }
    if (wifiSvc_ && wifiSvc_->requestReconnect) {
        wifiSvc_->requestReconnect(wifiSvc_->ctx);
    }
    return true;
}

bool WifiProvisioningModule::notifyShutdownPending_()
{
    shutdownPending_ = true;
    apRestartPending_ = false;
    apStopEventPending_ = false;
    portalLatched_ = false;
    if (apActive_) {
        LOGI("Provisioning shutdown pending; AP restart disabled");
    }
    return true;
}

void WifiProvisioningModule::onEventStatic_(const Event& e, void* user)
{
    WifiProvisioningModule* self = static_cast<WifiProvisioningModule*>(user);
    if (self) self->onEvent_(e);
}

void WifiProvisioningModule::onEvent_(const Event& e)
{
    if (e.id != EventId::NetworkShutdownPending) return;
    (void)notifyShutdownPending_();
}

void WifiProvisioningModule::buildApCredentials_()
{
    const uint64_t chipId = ESP.getEfuseMac();
    //const uint8_t b0 = (uint8_t)(chipId >> 16);
    //const uint8_t b1 = (uint8_t)(chipId >> 8);
    //const uint8_t b2 = (uint8_t)(chipId >> 0);
    //snprintf(apSsid_, sizeof(apSsid_), "flow.io-%02X%02X%02X", b0, b1, b2);
    uint64_t id=0;
    for(int i=0; i<17; i=i+8) id |= ((chipId >> (40 - i)) & 0xff) << i;
    snprintf(apSsid_, sizeof(apSsid_), "flow.io-%06X", id);
    snprintf(apPass_, sizeof(apPass_), "%s", kDefaultApPass);
}

void WifiProvisioningModule::refreshWifiConfig_()
{
    if (!cfgStore_) return;

    ethernetEnabled_ = false;
    char ethernetJson[96] = {0};
    if (cfgStore_->toJsonModule("ethernet", ethernetJson, sizeof(ethernetJson), nullptr)) {
        StaticJsonDocument<96> ethDoc;
        if (deserializeJson(ethDoc, ethernetJson) == DeserializationError::Ok && ethDoc.is<JsonObjectConst>()) {
            JsonObjectConst ethRoot = ethDoc.as<JsonObjectConst>();
            ethernetEnabled_ = ethRoot["enabled"] | false;
        }
    }

    if (ethernetEnabled_) {
        fastPortalStart_ = false;
    }

    char wifiJson[320] = {0};
    if (!cfgStore_->toJsonModule("wifi", wifiJson, sizeof(wifiJson), nullptr)) {
        wifiConfigured_ = false;
        wifiEnabled_ = true;
        wifiSsidLen_ = 0;
        wifiPassLen_ = 0;
        fastPortalStart_ = !ethernetEnabled_;
        networkManager_.updateConfig(ethernetEnabled_, wifiEnabled_, wifiConfigured_);
        return;
    }

    StaticJsonDocument<320> doc;
    const DeserializationError err = deserializeJson(doc, wifiJson);
    if (err || !doc.is<JsonObjectConst>()) {
        LOGW("Cannot parse wifi config for provisioning");
        wifiConfigured_ = false;
        wifiEnabled_ = true;
        wifiSsidLen_ = 0;
        wifiPassLen_ = 0;
        fastPortalStart_ = !ethernetEnabled_;
        networkManager_.updateConfig(ethernetEnabled_, wifiEnabled_, wifiConfigured_);
        return;
    }

    JsonObjectConst root = doc.as<JsonObjectConst>();
    const char* ssid = root["ssid"] | "";
    const char* pass = root["pass"] | "";
    wifiEnabled_ = root["enabled"] | true;
    wifiSsidLen_ = (uint8_t)strnlen(ssid ? ssid : "", 32U);
    wifiPassLen_ = (uint8_t)strnlen(pass ? pass : "", 64U);
    wifiConfigured_ = wifiEnabled_ && ssid && ssid[0] != '\0';
    fastPortalStart_ = !ethernetEnabled_ && wifiEnabled_ && !wifiConfigured_;
    networkManager_.updateConfig(ethernetEnabled_, wifiEnabled_, wifiConfigured_);
}

NetworkPortalReason WifiProvisioningModule::evaluatePortalReason_() const
{
    return networkManager_.portalReason(millis());
}

bool WifiProvisioningModule::startCaptivePortal_(NetworkPortalReason reason)
{
    if (apActive_) return true;
    syncNetworkManagerState_();
    if (networkManager_.hasNetwork()) return false;

    const uint32_t now = millis();
    if ((int32_t)(now - nextApStartAttemptMs_) < 0) {
        return false;
    }
    const uint32_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if ((now - lastApStartPrecheckLogMs_) >= kApStartLogIntervalMs) {
        lastApStartPrecheckLogMs_ = now;
        LOGI("Provisioning AP start precheck mode=%s wl=%d internal_free=%lu largest_internal=%lu",
             wifiModeName_(WiFi.getMode()),
             (int)WiFi.status(),
             (unsigned long)internalFree,
             (unsigned long)internalLargest);
    }
    const bool lowHeap = internalLargest < kApStartMinLargestInternalBytes;
    if (lowHeap) {
        ++apStartDeferredCount_;
        const bool forcedAttempt = (apStartDeferredCount_ >= kApStartForceAfterDefers);
        if (!forcedAttempt) {
            nextApStartAttemptMs_ = now + kApStartRetryMs;
        }
        if ((now - lastApStartDeferredLogMs_) >= kApStartLogIntervalMs) {
            lastApStartDeferredLogMs_ = now;
            LOGE("Provisioning AP start deferred: internal heap too low largest=%lu threshold=%lu deferred=%u",
                 (unsigned long)internalLargest,
                 (unsigned long)kApStartMinLargestInternalBytes,
                 (unsigned)apStartDeferredCount_);
        }
        if (!forcedAttempt) {
            return false;
        }
        LOGW("Provisioning AP forcing low-heap start largest=%lu deferred=%u threshold=%lu",
             (unsigned long)internalLargest,
             (unsigned)apStartDeferredCount_,
             (unsigned long)kApStartMinLargestInternalBytes);
    } else {
        apStartDeferredCount_ = 0;
    }

    if (wifiSvc_ && wifiSvc_->setStaRetryEnabled) {
        (void)wifiSvc_->setStaRetryEnabled(wifiSvc_->ctx, false);
    }
    if (wifiSvc_ && wifiSvc_->requestReconnect) {
        (void)wifiSvc_->requestReconnect(wifiSvc_->ctx);
    }
    delay(80);
    (void)WiFi.disconnect(false, false);
    (void)WiFi.scanDelete();

    // Force strict AP mode during provisioning: avoids STA scans/channel moves
    // that can break client association on some phones.
    (void)WiFi.enableSTA(false);
    const uint32_t staStopWaitStartMs = millis();
    while ((WiFi.getMode() & WIFI_MODE_STA) != 0 &&
           (millis() - staStopWaitStartMs) < kStaStopWaitMs) {
        delay(20);
    }
    if ((WiFi.getMode() & WIFI_MODE_STA) != 0) {
        LOGW("Provisioning AP start: STA still enabled after wait mode=%s wl=%d",
             wifiModeName_(WiFi.getMode()),
             (int)WiFi.status());
    }

    // Reset WiFi mode cleanly before softAP start, then expose the AP only
    // after Arduino/IDF have finished their setup events.
    const wifi_mode_t modeBefore = WiFi.getMode();
    if (modeBefore != WIFI_MODE_NULL) {
        const bool nullOk = WiFi.mode(WIFI_MODE_NULL);
        if (!nullOk) {
            LOGW("Provisioning AP reset to NULL failed current=%s", wifiModeName_(WiFi.getMode()));
        }
        delay(kModeResetSettleMs);
    }
    apStarting_ = true;
    const bool ok = WiFi.softAP(apSsid_,
                                apPass_,
                                kProvisioningApChannel,
                                0,
                                kProvisioningApMaxConnections,
                                false,
                                kProvisioningApAuthMode,
                                kProvisioningApCipher);
    if (!ok) {
        apStarting_ = false;
        LOGE("Cannot start AP portal");
        nextApStartAttemptMs_ = millis() + kApStartRetryMs;
        return false;
    }

    delay(kApStartStableDelayMs);
    apStarting_ = false;
    const wifi_mode_t modeAfterStart = WiFi.getMode();
    if ((modeAfterStart & WIFI_MODE_AP) == 0) {
        LOGE("Provisioning AP start unstable: AP mode missing after settle mode=%s wl=%d",
             wifiModeName_(modeAfterStart),
             (int)WiFi.status());
        nextApStartAttemptMs_ = millis() + kApStartRetryMs;
        return false;
    }

    wifi_config_t apCfg{};
    const esp_err_t apCfgErr = esp_wifi_get_config(WIFI_IF_AP, &apCfg);
    if (apCfgErr == ESP_OK) {
        char appliedSsid[33] = {0};
        const size_t apSsidLen = (size_t)((apCfg.ap.ssid_len <= 32U) ? apCfg.ap.ssid_len : 32U);
        memcpy(appliedSsid, apCfg.ap.ssid, apSsidLen);
        appliedSsid[apSsidLen] = '\0';
        const size_t appliedPassLen = strnlen((const char*)apCfg.ap.password, sizeof(apCfg.ap.password));
        LOGI("Provisioning AP config applied ssid='%s' ssid_len=%u pass_len=%u auth=%s(%u) cipher=%s(%u) ch=%u hidden=%u max_conn=%u",
             appliedSsid,
             (unsigned)apCfg.ap.ssid_len,
             (unsigned)appliedPassLen,
             wifiAuthName_(apCfg.ap.authmode),
             (unsigned)apCfg.ap.authmode,
             wifiCipherName_((wifi_cipher_type_t)apCfg.ap.pairwise_cipher),
             (unsigned)apCfg.ap.pairwise_cipher,
             (unsigned)apCfg.ap.channel,
             (unsigned)apCfg.ap.ssid_hidden,
             (unsigned)apCfg.ap.max_connection);
    } else {
        LOGE("Provisioning AP config read failed err=%d", (int)apCfgErr);
    }

    const IPAddress apIp = WiFi.softAPIP();
    if (apIp[0] == 0 && apIp[1] == 0 && apIp[2] == 0 && apIp[3] == 0) {
        LOGE("Provisioning AP start unstable: AP IP is 0.0.0.0");
        nextApStartAttemptMs_ = millis() + kApStartRetryMs;
        return false;
    }
    dns_.start(kDnsPort, "*", apIp);
    apActive_ = true;
    networkManager_.setCaptivePortalRunning(true);
    setHmiCaptivePortalCondition_(true);
    staProbeActive_ = false;
    apClientEverSeen_ = false;
    lastStaProbeStartMs_ = millis();
    nextApStartAttemptMs_ = 0;
    apStartDeferredCount_ = 0;
    refreshApClientState_(lastStaProbeStartMs_, false);

    const char* reasonTxt = (reason == NetworkPortalReason::MissingCredentials) ? "missing credentials" : "connect timeout";
    LOGW("[NET] No network available, starting captive portal (%s) SSID=%s pass_len=%u auth=%s cipher=%s channel=%u max_conn=%u IP=%u.%u.%u.%u",
         reasonTxt,
         apSsid_,
         (unsigned)strlen(apPass_),
         wifiAuthName_(kProvisioningApAuthMode),
         wifiCipherName_(kProvisioningApCipher),
         (unsigned)kProvisioningApChannel,
         (unsigned)kProvisioningApMaxConnections,
         apIp[0], apIp[1], apIp[2], apIp[3]);
    return true;
}

void WifiProvisioningModule::stopCaptivePortal_(const char* reason)
{
    if (!apActive_) return;

    stopStaProbe_(reason ? reason : "network available");
    if (wifiSvc_ && wifiSvc_->setStaRetryEnabled) {
        (void)wifiSvc_->setStaRetryEnabled(wifiSvc_->ctx, true);
    }
    dns_.stop();
    WiFi.softAPdisconnect(true);
    apActive_ = false;
    apClientCount_ = 0;
    lastApClientSeenMs_ = 0;
    lastApClientPollMs_ = 0;
    networkManager_.setCaptivePortalRunning(false);
    setHmiCaptivePortalCondition_(false);
    LOGI("[NET] Captive portal stopped because %s", reason ? reason : "network is available");
}

void WifiProvisioningModule::onWifiEventSys_(arduino_event_t* event)
{
    if (!event) return;
    WifiProvisioningModule* self = gWifiProvisioningInstance;
    if (!self) return;
    self->onWifiEvent_(event);
}

void WifiProvisioningModule::onWifiEvent_(arduino_event_t* event)
{
    if (!event) return;
    switch (event->event_id) {
    case ARDUINO_EVENT_WIFI_AP_START:
        if (apStarting_) {
            apStartDuringStartEventPending_ = true;
        }
        break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
        if (apStarting_) {
            apStopDuringStartEventPending_ = true;
        } else if (apActive_) {
            apStopEventPending_ = true;
        }
        break;
    case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED: {
        apProbeEventCount_ = apProbeEventCount_ + 1U;
        apProbeLastRssi_ = (int)event->event_info.wifi_ap_probereqrecved.rssi;
        break;
    }
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
        apClientConnectedEventPending_ = true;
        apClientRefreshPending_ = true;
        break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
        const uint8_t reason = (uint8_t)event->event_info.wifi_ap_stadisconnected.reason;
        apClientDisconnectReason_ = reason;
        apClientDisconnectedEventPending_ = true;
        apClientRefreshPending_ = true;
        break;
    }
    default:
        break;
    }
}

void WifiProvisioningModule::refreshApClientState_(uint32_t nowMs, bool fromEvent)
{
    if (!apActive_) {
        apClientCount_ = 0;
        return;
    }

    const uint8_t count = WiFi.softAPgetStationNum();
    if (count > 0) {
        lastApClientSeenMs_ = nowMs;
        apClientEverSeen_ = true;
    }
    if (count != apClientCount_) {
        apClientCount_ = count;
        if (fromEvent) {
            LOGI("AP clients changed (event) count=%u", (unsigned)apClientCount_);
        } else {
            LOGI("AP clients changed (poll) count=%u", (unsigned)apClientCount_);
        }
    }
}

void WifiProvisioningModule::startStaProbe_(uint32_t nowMs)
{
    if (staProbeActive_) return;
    if (!wifiEnabled_ || !wifiConfigured_) return;
    if (!apClientEverSeen_) return;

    staProbeActive_ = true;
    lastStaProbeStartMs_ = nowMs;
    if (wifiSvc_ && wifiSvc_->setStaRetryEnabled) {
        (void)wifiSvc_->setStaRetryEnabled(wifiSvc_->ctx, true);
    }
    if (wifiSvc_ && wifiSvc_->requestReconnect) {
        (void)wifiSvc_->requestReconnect(wifiSvc_->ctx);
    }
    LOGI("STA probe started window_ms=%lu ap_clients=%u",
         (unsigned long)kStaProbeWindowMs,
         (unsigned)apClientCount_);
}

void WifiProvisioningModule::stopStaProbe_(const char* reason)
{
    if (!staProbeActive_) return;
    staProbeActive_ = false;

    if (!isStaConnected_()) {
        if (wifiSvc_ && wifiSvc_->setStaRetryEnabled) {
            (void)wifiSvc_->setStaRetryEnabled(wifiSvc_->ctx, false);
        }
        if (wifiSvc_ && wifiSvc_->requestReconnect) {
            (void)wifiSvc_->requestReconnect(wifiSvc_->ctx);
        }
        delay(120);
        if (apActive_ && WiFi.getMode() != WIFI_MODE_AP) {
            LOGI("STA probe forcing AP mode from mode=%s wl=%d",
                 wifiModeName_(WiFi.getMode()),
                 (int)WiFi.status());
            (void)WiFi.mode(WIFI_MODE_AP);
        }
    }

    LOGI("STA probe stopped reason=%s", reason ? reason : "unknown");
}

void WifiProvisioningModule::handleStaProbePolicy_(uint32_t nowMs)
{
    if (!apActive_) return;
    (void)nowMs;
    // Keep provisioning AP in strict AP-only mode until user credentials are
    // updated; do not probe STA in background.
    return;
}

bool WifiProvisioningModule::isStaConnected_() const
{
    if (!wifiSvc_ || !wifiSvc_->isConnected) return false;
    return wifiSvc_->isConnected(wifiSvc_->ctx);
}

bool WifiProvisioningModule::hasStationNetwork_() const
{
    if (isStaConnected_()) return true;
    const NetworkAccessService* net = observedNetAccessSvc_;
    if (!net || !net->mode) return false;
    return net->mode(net->ctx) == NetworkAccessMode::Station;
}

void WifiProvisioningModule::syncNetworkManagerState_()
{
    const bool wifiHasIP = isStaConnected_();
    bool stationNetwork = wifiHasIP;
    if (observedNetAccessSvc_ && observedNetAccessSvc_->mode) {
        stationNetwork = observedNetAccessSvc_->mode(observedNetAccessSvc_->ctx) == NetworkAccessMode::Station;
    }
    const bool ethHasIP = ethernetEnabled_ && stationNetwork && !wifiHasIP;
    networkManager_.updateConfig(ethernetEnabled_, wifiEnabled_, wifiConfigured_);
    networkManager_.updateInterfaces(ethHasIP, wifiHasIP);
    networkManager_.setCaptivePortalRunning(apActive_);
}

void WifiProvisioningModule::setHmiCaptivePortalCondition_(bool active, bool force)
{
    const uint32_t now = millis();
    if (!force &&
        hmiCaptivePortalConditionSynced_ &&
        hmiCaptivePortalActive_ == active) {
        return;
    }
    if (!hmiSvc_ && services_) {
        hmiSvc_ = services_->get<HmiService>(ServiceId::Hmi);
    }
    if (!hmiSvc_) {
        hmiCaptivePortalConditionSynced_ = false;
        return;
    }
    if (hmiSvc_->setLedCondition) {
        (void)hmiSvc_->setLedCondition(hmiSvc_->ctx, HmiLedCondition::CaptivePortalActive, active);
        hmiCaptivePortalConditionSynced_ = true;
        lastHmiCaptivePortalSyncMs_ = now;
    } else {
        hmiCaptivePortalConditionSynced_ = false;
        return;
    }
    hmiCaptivePortalActive_ = active;
}

bool WifiProvisioningModule::getStaIp_(char* out, size_t len) const
{
    if (!out || len == 0) return false;
    if (!wifiSvc_ || !wifiSvc_->getIP) {
        out[0] = '\0';
        return false;
    }
    return wifiSvc_->getIP(wifiSvc_->ctx, out, len);
}

bool WifiProvisioningModule::getApIp_(char* out, size_t len) const
{
    if (!out || len == 0) return false;
    const IPAddress ip = WiFi.softAPIP();
    if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) {
        out[0] = '\0';
        return false;
    }
    snprintf(out, len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return true;
}


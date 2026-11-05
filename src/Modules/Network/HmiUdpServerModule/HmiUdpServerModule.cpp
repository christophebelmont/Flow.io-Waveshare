#include "Modules/Network/HmiUdpServerModule/HmiUdpServerModule.h"

#include <string.h>
#include <WiFi.h>
#include "Modules/Network/WifiModule/WifiRuntime.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::HmiUdpServerModule)
#include "Core/ModuleLog.h"

namespace {

static bool versionMajorEquals_(const char* version, uint8_t expected)
{
    if (!version || version[0] < '0' || version[0] > '9') return false;
    uint16_t major = 0U;
    size_t pos = 0U;
    while (version[pos] >= '0' && version[pos] <= '9') {
        major = (uint16_t)(major * 10U + (uint16_t)(version[pos] - '0'));
        if (major > 99U) return false;
        ++pos;
    }
    return pos > 0U && version[pos] == '.' && major == expected;
}

} // namespace

void HmiUdpServerModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    if (!buffers_) buffers_ = makeSpiRamObject<PacketBuffers>();
    if (!buffers_) LOGE("HMI UDP packet buffer PSRAM allocation failed");
    else LOGI("HMI UDP packet buffers bytes=%u memory=psram", (unsigned)sizeof(PacketBuffers));
    cfg.registerVar(tokenVar_);
    wifiSvc_ = services.get<WifiService>(ServiceId::Wifi);
    const DataStoreService* dsSvc = services.get<DataStoreService>(ServiceId::DataStore);
    dataStore_ = dsSvc ? dsSvc->store : nullptr;
}

bool HmiUdpServerModule::begin()
{
    if (!buffers_) return false;
    if (!wifiConnected_()) {
        if (started_) {
            udp_.stop();
            started_ = false;
            LOGW("HMI UDP socket stopped because network is offline");
        }
        return false;
    }
    if (started_) return true;

    if (!udp_.begin(HMI_UDP_PORT)) {
        LOGW("HMI UDP begin failed port=%u", (unsigned)HMI_UDP_PORT);
        return false;
    }
    started_ = true;
    LOGD("HMI UDP server listening port=%u", (unsigned)HMI_UDP_PORT);
    return true;
}

void HmiUdpServerModule::tick(uint32_t nowMs)
{
    if (!begin()) return;
    readUdp_(nowMs);
    serviceReliableTx_(nowMs);
    const uint32_t offlineTimeout = displaySleeping_ ? SleepOfflineTimeoutMs : OfflineTimeoutMs;
    if (displayOnline_ && (uint32_t)(nowMs - lastSeenMs_) >= offlineTimeout) {
        markDisplayOffline_("heartbeat-timeout", true);
        // A WiFiUDP socket can stop receiving after an interface transition or
        // a long period without a peer. Rebind at lease expiry so the next
        // broadcast Hello can always establish a fresh session.
        udp_.stop();
        started_ = false;
        if (!begin()) {
            LOGW("HMI UDP socket rebind failed after heartbeat timeout");
        } else {
            LOGW("HMI UDP socket reset after display link loss (heartbeat timeout)");
        }
    }
}

bool HmiUdpServerModule::consumeFullRefreshRequested()
{
    const bool requested = fullRefreshRequested_;
    fullRefreshRequested_ = false;
    return requested;
}

bool HmiUdpServerModule::consumeDisplayVersionChanged()
{
    const bool changed = displayVersionChangedPending_;
    displayVersionChangedPending_ = false;
    return changed;
}

bool HmiUdpServerModule::isLegacyV2() const
{
    return displayVersionDetected_ && versionMajorEquals_(displayVersion_, 2U);
}

bool HmiUdpServerModule::sendHomeText(HmiHomeTextField field, const char* text)
{
    if (shouldSuppressUiTx_()) return true;
    HmiUdpHomeTextPayload payload{};
    payload.field = (uint8_t)field;
    copyText_(payload.text, sizeof(payload.text), text);
    return sendPacket_(HmiUdpMsgType::HomeText, &payload, sizeof(payload));
}

bool HmiUdpServerModule::sendHomeGauge(HmiHomeGaugeField field, uint16_t percent)
{
    if (shouldSuppressUiTx_()) return true;
    HmiUdpHomeGaugePayload payload{};
    payload.field = (uint8_t)field;
    payload.percent = percent;
    return sendPacket_(HmiUdpMsgType::HomeGauge, &payload, sizeof(payload));
}

bool HmiUdpServerModule::sendHomeV2Needles(const NextionV2NeedlePublish& publish)
{
    if (shouldSuppressUiTx_()) return true;
    HmiUdpHomeV2NeedlesPayload payload{};
    if (publish.ph) payload.flags |= HMI_UDP_V2_NEEDLE_PH;
    if (publish.orp) payload.flags |= HMI_UDP_V2_NEEDLE_ORP;
    if (publish.psi) payload.flags |= HMI_UDP_V2_NEEDLE_PSI;
    payload.phNeedle = publish.phNeedle;
    payload.orpNeedle = publish.orpNeedle;
    payload.psiNeedle = publish.psiNeedle;
    return sendPacket_(HmiUdpMsgType::HomeV2Needles, &payload, sizeof(payload));
}

bool HmiUdpServerModule::sendHomeStateBits(uint32_t stateBits)
{
    if (shouldSuppressUiTx_()) return true;
    if (!started_ || !displayOnline_ || !wifiConnected_()) return true;
    HmiUdpStateBitsPayload payload{};
    payload.stateBits = stateBits;
    const bool ok = sendPacket_(HmiUdpMsgType::HomeStateBits, &payload, sizeof(payload), HMI_UDP_FLAG_ACK_REQUIRED);
    if (ok) {
        LOGD("HMI UDP send HomeStateBits stateBits=0x%08lX", (unsigned long)stateBits);
    }
    return ok;
}

bool HmiUdpServerModule::sendHomeAlarmBits(uint32_t alarmBits)
{
    if (shouldSuppressUiTx_()) return true;
    HmiUdpAlarmBitsPayload payload{};
    payload.alarmBits = alarmBits;
    return sendPacket_(HmiUdpMsgType::HomeAlarmBits, &payload, sizeof(payload));
}

bool HmiUdpServerModule::sendConfigStart(const ConfigMenuView& view)
{
    if (shouldSuppressUiTx_()) return true;
    clearReliableQueue_(true);
    HmiUdpConfigStartPayload payload{};
    payload.page = (uint8_t)(view.pageIndex + 1U);
    payload.pageCount = view.pageCount;
    payload.contextRef = view.contextRef;
    if (view.canHome) payload.flags |= HMI_UDP_CONFIG_VIEW_CAN_HOME;
    if (view.canBack) payload.flags |= HMI_UDP_CONFIG_VIEW_CAN_BACK;
    if (view.canValidate) payload.flags |= HMI_UDP_CONFIG_VIEW_CAN_VALIDATE;
    if (view.isHome) payload.flags |= HMI_UDP_CONFIG_VIEW_IS_HOME;
    copyText_(payload.title, sizeof(payload.title), view.breadcrumb);
    return enqueueReliable_(HmiUdpMsgType::ConfigStart, &payload, sizeof(payload));
}

bool HmiUdpServerModule::sendConfigRow(uint8_t row, const ConfigMenuRowView& viewRow, ConfigMenuMode mode)
{
    if (shouldSuppressUiTx_()) return true;
    HmiUdpConfigRowPayload payload{};
    fillConfigRowPayload_(payload, row, viewRow, mode);
    return enqueueReliable_(HmiUdpMsgType::ConfigRow, &payload, sizeof(payload));
}

bool HmiUdpServerModule::sendConfigEnd(uint8_t rowCount)
{
    if (shouldSuppressUiTx_()) return true;
    HmiUdpConfigEndPayload payload{};
    payload.rowCount = rowCount;
    return enqueueReliable_(HmiUdpMsgType::ConfigEnd, &payload, sizeof(payload));
}

bool HmiUdpServerModule::sendConfigViewSnapshot(const ConfigMenuView& view)
{
    if (shouldSuppressUiTx_()) return true;
    HmiUdpConfigViewSnapshotPayload payload{};
    payload.page = (uint8_t)(view.pageIndex + 1U);
    payload.pageCount = view.pageCount;
    payload.rowCount = view.rowCountOnPage;
    payload.mode = (uint8_t)view.mode;
    payload.contextRef = view.contextRef;
    if (view.canHome) payload.flags |= HMI_UDP_CONFIG_VIEW_CAN_HOME;
    if (view.canBack) payload.flags |= HMI_UDP_CONFIG_VIEW_CAN_BACK;
    if (view.canValidate) payload.flags |= HMI_UDP_CONFIG_VIEW_CAN_VALIDATE;
    if (view.isHome) payload.flags |= HMI_UDP_CONFIG_VIEW_IS_HOME;
    copyText_(payload.title, sizeof(payload.title), view.breadcrumb);
    for (uint8_t i = 0; i < HMI_UDP_CONFIG_SNAPSHOT_ROWS; ++i) {
        fillConfigRowPayload_(payload.rows[i], i, view.rows[i], view.mode);
    }
    return sendReliableLarge_(HmiUdpMsgType::ConfigViewSnapshot, &payload, sizeof(payload));
}

bool HmiUdpServerModule::sendRtcWrite(const HmiRtcDateTime& value)
{
    if (shouldSuppressUiTx_()) return false;
    HmiUdpRtcPayload payload{};
    hmiUdpRtcToPayload(value, payload);
    return sendPacket_(HmiUdpMsgType::RtcWrite, &payload, sizeof(payload), HMI_UDP_FLAG_ACK_REQUIRED);
}

bool HmiUdpServerModule::requestRtcRead()
{
    if (displaySleeping_) return false;
    return sendPacket_(HmiUdpMsgType::RtcReadRequest, nullptr, 0U, HMI_UDP_FLAG_ACK_REQUIRED);
}

bool HmiUdpServerModule::requestRtcRead(HmiRtcDateTime& out, uint16_t timeoutMs)
{
    out = HmiRtcDateTime{};
    rtcResponseReady_ = false;
    rtcResponse_ = HmiRtcDateTime{};
    if (!requestRtcRead()) return false;

    rtcReadInProgress_ = true;
    if (displayOnline_) lastSeenMs_ = millis();
    const uint32_t start = millis();
    while ((uint32_t)(millis() - start) < (uint32_t)timeoutMs) {
        const uint32_t now = millis();
        readUdp_(now);
        serviceReliableTx_(now);
        if (rtcResponseReady_) {
            out = rtcResponse_;
            rtcResponseReady_ = false;
            rtcReadInProgress_ = false;
            return true;
        }
        delay(1);
    }
    rtcReadInProgress_ = false;
    return false;
}

bool HmiUdpServerModule::pollEvent(HmiEvent& out)
{
    out = HmiEvent{};
    if (eventHead_ == eventTail_) return false;
    out = eventQueue_[eventTail_];
    eventTail_ = (uint8_t)((eventTail_ + 1U) % HMI_UDP_EVENT_QUEUE_SIZE);
    return true;
}

bool HmiUdpServerModule::sendPacket_(HmiUdpMsgType type, const void* payload, uint8_t payloadLen, uint8_t flags)
{
    if ((flags & HMI_UDP_FLAG_ACK_REQUIRED) != 0U) {
        return enqueueReliable_(type, payload, payloadLen);
    }
    return sendImmediate_(type, payload, payloadLen, flags);
}

bool HmiUdpServerModule::sendImmediate_(HmiUdpMsgType type, const void* payload, uint8_t payloadLen, uint8_t flags)
{
    if (!started_ || !displayOnline_ || !wifiConnected_()) return false;

    size_t packetLen = 0;
    if (!hmiUdpBuildPacket(buffers_->tx,
                           sizeof(buffers_->tx),
                           packetLen,
                           type,
                           txSeq_++,
                           lastRxSeq_,
                           flags,
                           payload,
                           payloadLen)) {
        return false;
    }
    if (!udp_.beginPacket(remoteIp_, remotePort_)) return false;
    const size_t written = udp_.write(buffers_->tx, packetLen);
    return written == packetLen && udp_.endPacket() == 1;
}

bool HmiUdpServerModule::enqueueReliable_(HmiUdpMsgType type, const void* payload, uint8_t payloadLen)
{
    if (!started_ || !displayOnline_ || !wifiConnected_()) return false;
    if (payloadLen > HMI_UDP_OUT_PAYLOAD_MAX) return false;

    if (type == HmiUdpMsgType::HomeStateBits) {
        uint8_t idx = outTail_;
        while (idx != outHead_) {
            OutPacket& queued = buffers_->outgoing[idx];
            if (queued.type == type) {
                queued.len = payloadLen;
                if (payloadLen > 0U && payload) {
                    memcpy(queued.payload, payload, payloadLen);
                }
                return true;
            }
            idx = (uint8_t)((idx + 1U) % HMI_UDP_OUT_QUEUE_SIZE);
        }
    }

    const uint8_t next = (uint8_t)((outHead_ + 1U) % HMI_UDP_OUT_QUEUE_SIZE);
    if (next == outTail_) {
        LOGW("HMI UDP reliable TX queue full type=%u len=%u", (unsigned)type, (unsigned)payloadLen);
        return false;
    }

    OutPacket& pkt = buffers_->outgoing[outHead_];
    pkt.type = type;
    pkt.len = payloadLen;
    if (payloadLen > 0U && payload) {
        memcpy(pkt.payload, payload, payloadLen);
    }
    outHead_ = next;
    return true;
}

bool HmiUdpServerModule::sendReliableLarge_(HmiUdpMsgType type, const void* payload, size_t payloadLen)
{
    if (!started_ || !displayOnline_ || !wifiConnected_()) return false;
    if (payloadLen > HMI_UDP_LARGE_PAYLOAD_MAX) return false;
    if (payloadLen > 0U && !payload) return false;

    if (reliablePendingLen_ > 0U) {
        const size_t pendingPayloadLen = reliablePendingLen_ > sizeof(HmiUdpHeader)
            ? reliablePendingLen_ - sizeof(HmiUdpHeader)
            : 0U;
        const uint8_t* pendingPayload = buffers_->reliablePending + sizeof(HmiUdpHeader);
        const bool samePending = reliablePendingType_ == type &&
                                 pendingPayloadLen == payloadLen &&
                                 (payloadLen == 0U || memcmp(pendingPayload, payload, payloadLen) == 0);
        if (samePending) {
            LOGD("HMI UDP reliable large duplicate suppressed type=%u seq=%u len=%u",
                 (unsigned)type,
                 (unsigned)reliablePendingSeq_,
                 (unsigned)payloadLen);
            return true;
        }

        LOGD("HMI UDP reliable large busy type=%u pending=%u seq=%u len=%u",
             (unsigned)type,
             (unsigned)reliablePendingType_,
             (unsigned)reliablePendingSeq_,
             (unsigned)payloadLen);
        return false;
    }

    clearReliableQueue_(false);
    if (!buildReliablePending_(type, payload, payloadLen)) return false;
    serviceReliableTx_(millis());
    return reliablePendingLen_ > 0U;
}

bool HmiUdpServerModule::buildReliablePending_(HmiUdpMsgType type, const void* payload, size_t payloadLen)
{
    if (payloadLen > HMI_UDP_LARGE_PAYLOAD_MAX) return false;
    if (payloadLen > 0U && !payload) return false;

    reliablePendingSeq_ = txSeq_;
    reliablePendingType_ = type;
    reliableAttempts_ = 0;
    reliableLastSendMs_ = 0;

    size_t packetLen = 0;
    if (!hmiUdpBuildPacket(buffers_->reliablePending,
                           sizeof(buffers_->reliablePending),
                           packetLen,
                           type,
                           txSeq_++,
                           lastRxSeq_,
                           HMI_UDP_FLAG_ACK_REQUIRED,
                           payload,
                           payloadLen)) {
        reliablePendingSeq_ = 0;
        reliablePendingType_ = HmiUdpMsgType::Error;
        return false;
    }

    reliablePendingLen_ = packetLen;
    return true;
}

bool HmiUdpServerModule::loadNextReliable_()
{
    if (reliablePendingLen_ > 0 || outHead_ == outTail_) return false;

    const OutPacket& pkt = buffers_->outgoing[outTail_];
    reliablePendingSeq_ = txSeq_;
    reliablePendingType_ = pkt.type;
    reliableAttempts_ = 0;
    reliableLastSendMs_ = 0;

    size_t packetLen = 0;
    if (!hmiUdpBuildPacket(buffers_->reliablePending,
                           sizeof(buffers_->reliablePending),
                           packetLen,
                           pkt.type,
                           txSeq_++,
                           lastRxSeq_,
                           HMI_UDP_FLAG_ACK_REQUIRED,
                           pkt.len > 0U ? pkt.payload : nullptr,
                           pkt.len)) {
        reliablePendingSeq_ = 0;
        reliablePendingType_ = HmiUdpMsgType::Error;
        return false;
    }

    reliablePendingLen_ = packetLen;
    outTail_ = (uint8_t)((outTail_ + 1U) % HMI_UDP_OUT_QUEUE_SIZE);
    return true;
}

void HmiUdpServerModule::serviceReliableTx_(uint32_t nowMs)
{
    if (!started_ || !displayOnline_ || !wifiConnected_()) return;
    if (reliablePendingLen_ == 0) {
        (void)loadNextReliable_();
    }
    if (reliablePendingLen_ == 0) return;

    uint32_t retryDelayMs = ReliableRetryMs;
    if (reliableAttempts_ > 1U) {
        const uint8_t backoffShift = reliableAttempts_ >= 5U ? 4U : (uint8_t)(reliableAttempts_ - 1U);
        retryDelayMs <<= backoffShift;
        if (retryDelayMs > ReliableRetryMaxMs) retryDelayMs = ReliableRetryMaxMs;
    }
    if (reliableAttempts_ > 0U && (uint32_t)(nowMs - reliableLastSendMs_) < retryDelayMs) return;
    if (!udp_.beginPacket(remoteIp_, remotePort_)) return;
    const size_t written = udp_.write(buffers_->reliablePending, reliablePendingLen_);
    if (written == reliablePendingLen_ && udp_.endPacket() == 1) {
        if (reliableAttempts_ < UINT8_MAX) ++reliableAttempts_;
        reliableLastSendMs_ = nowMs;
        LOGD("HMI UDP reliable TX type=%u seq=%u attempt=%u len=%u",
             (unsigned)reliablePendingType_,
             (unsigned)reliablePendingSeq_,
             (unsigned)reliableAttempts_,
             (unsigned)reliablePendingLen_);
    }
}

void HmiUdpServerModule::clearReliableQueue_(bool clearPending)
{
    outHead_ = 0;
    outTail_ = 0;
    if (clearPending) {
        reliablePendingLen_ = 0;
        reliablePendingSeq_ = 0;
        reliablePendingType_ = HmiUdpMsgType::Error;
        reliableAttempts_ = 0;
        reliableLastSendMs_ = 0;
    }
}

void HmiUdpServerModule::markDisplayOffline_(const char* reason, bool clearPending)
{
    const bool wasOnline = displayOnline_;
    displayOnline_ = false;
    displaySleeping_ = false;
    displayVersionDetected_ = false;
    displayVersion_[0] = '\0';
    clearReliableQueue_(clearPending);
    if (wasOnline) {
        LOGI("HMI UDP display offline reason=%s", reason && reason[0] ? reason : "unknown");
    }
}

bool HmiUdpServerModule::isConfigMsg_(HmiUdpMsgType type) const
{
    return type == HmiUdpMsgType::ConfigStart ||
           type == HmiUdpMsgType::ConfigRow ||
           type == HmiUdpMsgType::ConfigEnd ||
           type == HmiUdpMsgType::ConfigValues ||
           type == HmiUdpMsgType::ConfigViewSnapshot;
}

bool HmiUdpServerModule::shouldSuppressUiTx_() const
{
    return displaySleeping_;
}

bool HmiUdpServerModule::sendAck_(uint16_t seq)
{
    if (!started_ || !buffers_) return false;
    size_t packetLen = 0;
    const uint16_t ackSeq = txSeq_++;
    if (!hmiUdpBuildPacket(buffers_->tx,
                           sizeof(buffers_->tx),
                           packetLen,
                           HmiUdpMsgType::Ack,
                           ackSeq,
                           seq,
                           HMI_UDP_FLAG_IS_ACK,
                           nullptr,
                           0U)) {
        LOGI("HMI UDP ACK TX build failed type=%u seq=%u ack=%u flags=%u src=%u.%u.%u.%u:%u dst=%u.%u.%u.%u:%u pending=%u",
             (unsigned)HmiUdpMsgType::Ack, (unsigned)ackSeq, (unsigned)seq, (unsigned)HMI_UDP_FLAG_IS_ACK,
             (unsigned)WiFi.localIP()[0], (unsigned)WiFi.localIP()[1], (unsigned)WiFi.localIP()[2], (unsigned)WiFi.localIP()[3], (unsigned)HMI_UDP_PORT,
             (unsigned)remoteIp_[0], (unsigned)remoteIp_[1], (unsigned)remoteIp_[2], (unsigned)remoteIp_[3], (unsigned)remotePort_, (unsigned)reliablePendingSeq_);
        return false;
    }
    const bool began = udp_.beginPacket(remoteIp_, remotePort_);
    size_t written = 0U;
    int ended = 0;
    if (began) {
        written = udp_.write(buffers_->tx, packetLen);
        ended = udp_.endPacket();
    }
    const bool sent = began && written == packetLen && ended == 1;
    const IPAddress localIp = WiFi.localIP();
    LOGI("HMI UDP ACK TX type=%u seq=%u ack=%u flags=%u src=%u.%u.%u.%u:%u dst=%u.%u.%u.%u:%u begin=%u write=%u/%u end=%d sent=%u pending=%u",
         (unsigned)HmiUdpMsgType::Ack, (unsigned)ackSeq, (unsigned)seq, (unsigned)HMI_UDP_FLAG_IS_ACK,
         (unsigned)localIp[0], (unsigned)localIp[1], (unsigned)localIp[2], (unsigned)localIp[3], (unsigned)HMI_UDP_PORT,
         (unsigned)remoteIp_[0], (unsigned)remoteIp_[1], (unsigned)remoteIp_[2], (unsigned)remoteIp_[3], (unsigned)remotePort_,
         (unsigned)began, (unsigned)written, (unsigned)packetLen, ended, (unsigned)sent, (unsigned)reliablePendingSeq_);
    return sent;
}

void HmiUdpServerModule::readUdp_(uint32_t nowMs)
{
    int packetSize = udp_.parsePacket();
    for (uint8_t packetsRead = 0U; packetSize > 0 && packetsRead < 4U; ++packetsRead) {
        if (packetSize <= (int)sizeof(buffers_->rx)) {
            const int len = udp_.read(buffers_->rx, sizeof(buffers_->rx));
            const HmiUdpHeader* header = nullptr;
            const uint8_t* payload = nullptr;
            if (len > 0 && hmiUdpValidatePacket(buffers_->rx, (size_t)len, header, payload)) {
                remoteIp_ = udp_.remoteIP();
                remotePort_ = udp_.remotePort();
                handlePacket_(*header, payload, nowMs);
            } else if (len >= (int)sizeof(HmiUdpHeader)) {
                const HmiUdpHeader& candidate = *reinterpret_cast<const HmiUdpHeader*>(buffers_->rx);
                if (candidate.type == (uint8_t)HmiUdpMsgType::Ack || (candidate.flags & HMI_UDP_FLAG_IS_ACK) != 0U) {
                    const IPAddress sourceIp = udp_.remoteIP();
                    const IPAddress localIp = WiFi.localIP();
                    LOGI("HMI UDP ACK RX invalid type=%u seq=%u ack=%u flags=%u src=%u.%u.%u.%u:%u dst=%u.%u.%u.%u:%u packetLen=%d pending=%u",
                         (unsigned)candidate.type, (unsigned)candidate.seq, (unsigned)candidate.ack, (unsigned)candidate.flags,
                         (unsigned)sourceIp[0], (unsigned)sourceIp[1], (unsigned)sourceIp[2], (unsigned)sourceIp[3], (unsigned)udp_.remotePort(),
                         (unsigned)localIp[0], (unsigned)localIp[1], (unsigned)localIp[2], (unsigned)localIp[3], (unsigned)HMI_UDP_PORT,
                         len, (unsigned)reliablePendingSeq_);
                }
            }
        } else {
            while (udp_.available() > 0) (void)udp_.read();
        }
        packetSize = udp_.parsePacket();
    }
}

void HmiUdpServerModule::handlePacket_(const HmiUdpHeader& header, const uint8_t* payload, uint32_t nowMs)
{
    if (!isValidInboundMessage_(header)) return;
    const bool sessionWasOnline = displayOnline_;
    const bool inboundSequenceRestarted = hasLastRxSeq_ &&
        (uint16_t)(header.seq - lastRxSeq_) >= 0x8000U;
    lastRxSeq_ = header.seq;
    hasLastRxSeq_ = true;
    markRemote_(nowMs);

    const HmiUdpMsgType type = (HmiUdpMsgType)header.type;
    if ((header.flags & HMI_UDP_FLAG_IS_ACK) != 0U || type == HmiUdpMsgType::Ack) {
        const bool matched = reliablePendingLen_ > 0 && header.ack == reliablePendingSeq_;
        const IPAddress sourceIp = udp_.remoteIP();
        const IPAddress localIp = WiFi.localIP();
        LOGI("HMI UDP ACK RX type=%u seq=%u ack=%u flags=%u src=%u.%u.%u.%u:%u dst=%u.%u.%u.%u:%u valid=1 pending=%u matched=%u",
             (unsigned)type, (unsigned)header.seq, (unsigned)header.ack, (unsigned)header.flags,
             (unsigned)sourceIp[0], (unsigned)sourceIp[1], (unsigned)sourceIp[2], (unsigned)sourceIp[3], (unsigned)udp_.remotePort(),
             (unsigned)localIp[0], (unsigned)localIp[1], (unsigned)localIp[2], (unsigned)localIp[3], (unsigned)HMI_UDP_PORT,
             (unsigned)reliablePendingSeq_, (unsigned)matched);
        if (matched) {
            LOGD("HMI UDP reliable ACK type=%u seq=%u attempts=%u",
                 (unsigned)reliablePendingType_,
                 (unsigned)reliablePendingSeq_,
                 (unsigned)reliableAttempts_);
            reliablePendingLen_ = 0;
            reliablePendingSeq_ = 0;
            reliablePendingType_ = HmiUdpMsgType::Error;
            reliableAttempts_ = 0;
            reliableLastSendMs_ = 0;
        }
        return;
    }

    // HmiEvent is acknowledged only after it is safely queued (or recognized as
    // a replay). Otherwise a full application queue could make the sender drop
    // an action that Flow.io never accepted.
    if ((header.flags & HMI_UDP_FLAG_ACK_REQUIRED) != 0U && type != HmiUdpMsgType::HmiEvent) {
        (void)sendAck_(header.seq);
    }

    switch (type) {
        case HmiUdpMsgType::Hello: {
            if (header.len != sizeof(HmiUdpHelloPayload)) return;
            const auto* hello = reinterpret_cast<const HmiUdpHelloPayload*>(payload);
            HmiUdpWelcomePayload welcome{};
            welcome.flowFw = 1U;
            welcome.protoVersion = HMI_UDP_VERSION;
            welcome.accepted = tokenAccepted_(hello ? hello->tokenCrc : 0U) ? 1U : 0U;
            const IPAddress sourceIp = udp_.remoteIP();
            LOGI("HMI UDP Hello RX seq=%u ack=%u source=%u.%u.%u.%u:%u accepted=%u",
                 (unsigned)header.seq,
                 (unsigned)header.ack,
                 (unsigned)sourceIp[0],
                 (unsigned)sourceIp[1],
                 (unsigned)sourceIp[2],
                 (unsigned)sourceIp[3],
                 (unsigned)udp_.remotePort(),
                 (unsigned)welcome.accepted);
            if (welcome.accepted == 0U) {
                (void)sendPacket_(HmiUdpMsgType::Welcome, &welcome, sizeof(welcome));
                displayOnline_ = false;
                displaySleeping_ = false;
                displayVersionDetected_ = false;
                displayVersion_[0] = '\0';
                return;
            }
            const bool haveDisplayVersion = hello &&
                                            (hello->flags & HMI_UDP_HELLO_FLAG_NEXTION_VERSION_VALID) != 0U;
            const bool nextionSleeping = hello &&
                                         (hello->flags & HMI_UDP_HELLO_FLAG_NEXTION_SLEEPING) != 0U;
            const bool wasOnline = sessionWasOnline;
            const bool previousVersionDetected = displayVersionDetected_;
            char previousVersion[HMI_DISPLAY_VERSION_TEXT_MAX]{};
            strncpy(previousVersion, displayVersion_, sizeof(previousVersion) - 1U);
            previousVersion[sizeof(previousVersion) - 1U] = '\0';
            const bool newDisplayProcess = header.ack == 0U &&
                ((!wasOnline && !hasLastEventSeq_) || inboundSequenceRestarted);
            const bool freshDisplaySession = !wasOnline || newDisplayProcess;
            if (freshDisplaySession) {
                // Discard display-bound data from the old link. The full refresh
                // below rebuilds the screen from authoritative Flow.io state.
                clearReliableQueue_(true);
                LOGI("HMI UDP display link resynchronized");
            }
            if (newDisplayProcess) {
                // A process restart resets the display sequence space. A mere
                // link outage does not: retain the deduplication window so an
                // event whose ACK was lost cannot execute twice after reconnect.
                eventHead_ = 0U;
                eventTail_ = 0U;
                hasLastEventSeq_ = false;
                lastEventSeq_ = 0U;
                LOGI("HMI UDP display process sequence window reset");
            }
            displayVersionDetected_ = haveDisplayVersion;
            if (haveDisplayVersion) {
                strncpy(displayVersion_, hello->nextionVersion, sizeof(displayVersion_) - 1U);
                displayVersion_[sizeof(displayVersion_) - 1U] = '\0';
            } else {
                displayVersion_[0] = '\0';
            }
            const bool versionChanged = previousVersionDetected != displayVersionDetected_ ||
                                        (displayVersionDetected_ && strcmp(previousVersion, displayVersion_) != 0);
            const bool displayVersionUpdated = !wasOnline || versionChanged;
            if (displayVersionUpdated) {
                displayVersionChangedPending_ = true;
            }
            const uint16_t fcdFw = hello ? hello->displayFw : 0U;
            const uint16_t fcdProto = hello ? hello->protoVersion : 0U;
            if (displayVersionUpdated) {
                if (displayVersionDetected_) {
                    LOGI("HMI UDP Flow Connect Display detected Nextion display version=%s fcd_fw=%u proto=%u",
                         displayVersion_,
                         (unsigned)fcdFw,
                         (unsigned)fcdProto);
                } else {
                    LOGI("HMI UDP Flow Connect Display detected Nextion display version unknown fcd_fw=%u proto=%u",
                         (unsigned)fcdFw,
                         (unsigned)fcdProto);
                }
            }
            displayOnline_ = true;
            displaySleeping_ = nextionSleeping;
            if (!nextionSleeping) {
                fullRefreshRequested_ = true;
                if (!wasOnline || freshDisplaySession) {
                    LOGI("HMI UDP Flow Connect Display fresh session detected");
                }
            }
            (void)sendPacket_(HmiUdpMsgType::Welcome, &welcome, sizeof(welcome));
            break;
        }
        case HmiUdpMsgType::Ping:
            (void)sendPacket_(HmiUdpMsgType::Pong, nullptr, 0U);
            break;
        case HmiUdpMsgType::FullRefresh:
            fullRefreshRequested_ = true;
            LOGD("HMI UDP full refresh requested by Flow Connect Display seq=%u", (unsigned)header.seq);
            break;
        case HmiUdpMsgType::HmiEvent: {
            if (header.len != sizeof(HmiUdpEventPayload) || !payload) return;
            const uint16_t eventSequenceDistance = (uint16_t)(header.seq - lastEventSeq_);
            if (hasLastEventSeq_ && (eventSequenceDistance == 0U || eventSequenceDistance >= 0x8000U)) {
                LOGD("HMI UDP duplicate event ignored seq=%u", (unsigned)header.seq);
                if ((header.flags & HMI_UDP_FLAG_ACK_REQUIRED) != 0U) (void)sendAck_(header.seq);
                return;
            }
            HmiEvent event{};
            hmiUdpPayloadToEvent(*reinterpret_cast<const HmiUdpEventPayload*>(payload), event);
            if (event.type == HmiEventType::DisplaySleep) {
                if ((header.flags & HMI_UDP_FLAG_ACK_REQUIRED) != 0U) (void)sendAck_(header.seq);
                lastEventSeq_ = header.seq;
                hasLastEventSeq_ = true;
                displaySleeping_ = true;
                clearReliableQueue_(true);
                LOGI("HMI UDP display sleeping");
                return;
            }
            if (event.type == HmiEventType::DisplayWake) {
                if ((header.flags & HMI_UDP_FLAG_ACK_REQUIRED) != 0U) (void)sendAck_(header.seq);
                lastEventSeq_ = header.seq;
                hasLastEventSeq_ = true;
                const bool wasSleeping = displaySleeping_;
                displaySleeping_ = false;
                fullRefreshRequested_ = true;
                LOGI("HMI UDP display awake%s", wasSleeping ? "" : " (already awake)");
                return;
            }
            if (pushEvent_(event)) {
                if ((header.flags & HMI_UDP_FLAG_ACK_REQUIRED) != 0U) (void)sendAck_(header.seq);
                lastEventSeq_ = header.seq;
                hasLastEventSeq_ = true;
                const bool menuEvent = event.type == HmiEventType::Back ||
                                       event.type == HmiEventType::Validate ||
                                       event.type == HmiEventType::NextPage ||
                                       event.type == HmiEventType::PrevPage ||
                                       event.type == HmiEventType::RowActivate ||
                                       event.type == HmiEventType::RowToggle ||
                                       event.type == HmiEventType::RowCycle ||
                                       event.type == HmiEventType::RowSetText ||
                                       event.type == HmiEventType::RowSetSlider ||
                                       event.type == HmiEventType::RowEdit ||
                                       event.type == HmiEventType::ConfigEnter ||
                                       event.type == HmiEventType::ConfigExit;
                if (menuEvent) {
                    LOGI("HMI UDP menu event queued seq=%u type=%u row=%u value=%u",
                         (unsigned)header.seq,
                         (unsigned)event.type,
                         (unsigned)event.row,
                         (unsigned)event.value);
                } else {
                    LOGD("HMI UDP event queued seq=%u type=%u command=%u value=%u row=%u",
                         (unsigned)header.seq,
                         (unsigned)event.type,
                         (unsigned)event.command,
                         (unsigned)event.value,
                         (unsigned)event.row);
                }
            } else {
                LOGW("HMI UDP event queue full seq=%u type=%u command=%u",
                     (unsigned)header.seq,
                     (unsigned)event.type,
                     (unsigned)event.command);
            }
            break;
        }
        case HmiUdpMsgType::RtcReadResponse: {
            if (header.len != sizeof(HmiUdpRtcPayload) || !payload) return;
            hmiUdpPayloadToRtc(*reinterpret_cast<const HmiUdpRtcPayload*>(payload), rtcResponse_);
            rtcResponseReady_ = true;
            LOGD("HMI UDP RTC response %u-%02u-%02u %02u:%02u:%02u",
                 (unsigned)rtcResponse_.year,
                 (unsigned)rtcResponse_.month,
                 (unsigned)rtcResponse_.day,
                 (unsigned)rtcResponse_.hour,
                 (unsigned)rtcResponse_.minute,
                 (unsigned)rtcResponse_.second);
            break;
        }
        default:
            break;
    }
}

void HmiUdpServerModule::markRemote_(uint32_t nowMs)
{
    if (!displayOnline_) {
        LOGI("HMI UDP display linked");
    }
    displayOnline_ = true;
    lastSeenMs_ = nowMs;
}

bool HmiUdpServerModule::isValidInboundMessage_(const HmiUdpHeader& header) const
{
    const HmiUdpMsgType type = (HmiUdpMsgType)header.type;
    switch (type) {
        case HmiUdpMsgType::Hello: return header.len == sizeof(HmiUdpHelloPayload);
        case HmiUdpMsgType::Ping:
        case HmiUdpMsgType::Pong:
        case HmiUdpMsgType::Ack:
        case HmiUdpMsgType::FullRefresh: return header.len == 0U;
        case HmiUdpMsgType::HmiEvent: return header.len == sizeof(HmiUdpEventPayload);
        case HmiUdpMsgType::RtcReadResponse: return header.len == sizeof(HmiUdpRtcPayload);
        default: return false;
    }
}

bool HmiUdpServerModule::pushEvent_(const HmiEvent& event)
{
    const uint8_t next = (uint8_t)((eventHead_ + 1U) % HMI_UDP_EVENT_QUEUE_SIZE);
    if (next == eventTail_) return false;
    eventQueue_[eventHead_] = event;
    eventHead_ = next;
    return true;
}

bool HmiUdpServerModule::wifiConnected_() const
{
    if (dataStore_) return networkReady(*dataStore_);
    return wifiSvc_ && wifiSvc_->isConnected && wifiSvc_->isConnected(wifiSvc_->ctx);
}

bool HmiUdpServerModule::tokenAccepted_(uint32_t tokenCrc) const
{
    if (cfgData_.token[0] == '\0') return true;
    return tokenCrc != 0U && tokenCrc == hmiUdpTokenCrc(cfgData_.token);
}

void HmiUdpServerModule::copyText_(char* out, size_t outLen, const char* in)
{
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!in) return;
    size_t i = 0;
    while (i + 1 < outLen && in[i] != '\0') {
        out[i] = in[i];
        ++i;
    }
    out[i] = '\0';
}

void HmiUdpServerModule::fillConfigRowPayload_(HmiUdpConfigRowPayload& payload,
                                               uint8_t row,
                                               const ConfigMenuRowView& viewRow,
                                               ConfigMenuMode mode)
{
    payload = HmiUdpConfigRowPayload{};
    payload.row = row;
    payload.widget = (uint8_t)viewRow.widget;
    payload.editType = viewRow.editType;
    if (viewRow.visible) payload.flags |= HMI_UDP_CONFIG_ROW_VISIBLE;
    if (viewRow.valueVisible) payload.flags |= HMI_UDP_CONFIG_ROW_VALUE_VISIBLE;
    if (viewRow.editable) payload.flags |= HMI_UDP_CONFIG_ROW_EDITABLE;
    if (viewRow.dirty) payload.flags |= HMI_UDP_CONFIG_ROW_DIRTY;
    if (viewRow.canEnter) payload.flags |= HMI_UDP_CONFIG_ROW_CAN_ENTER;
    if (viewRow.canEdit) payload.flags |= HMI_UDP_CONFIG_ROW_CAN_EDIT;
    if (mode == ConfigMenuMode::Edit) payload.flags |= HMI_UDP_CONFIG_MODE_EDIT;
    copyText_(payload.label, sizeof(payload.label), viewRow.label);
    copyText_(payload.value, sizeof(payload.value), viewRow.value);
}

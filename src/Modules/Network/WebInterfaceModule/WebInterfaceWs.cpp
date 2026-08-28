/**
 * @file WebInterfaceWs.cpp
 * @brief WebSocket transport and UART-to-web flow control for WebInterfaceModule.
 */

#include "WebInterfaceModule.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::WebInterfaceModule)
#include "Core/ModuleLog.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#ifndef FLOW_WEB_HEAP_FORENSICS
#define FLOW_WEB_HEAP_FORENSICS 0
#endif

namespace {
static constexpr uint32_t kWsMinFreeBytes = 12288U;
static constexpr uint32_t kWsMinLargestBytes = 3072U;

struct HeapForensicSnapshot {
    uint32_t freeBytes = 0;
    uint32_t minFreeBytes = 0;
    uint32_t largestFreeBlock = 0;
};

HeapForensicSnapshot captureHeapForensicSnapshot_()
{
    HeapForensicSnapshot snap{};
    snap.freeBytes = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    snap.minFreeBytes = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    snap.largestFreeBlock = snap.freeBytes;
    return snap;
}

const char* wsEventName_(AwsEventType type)
{
    switch (type) {
    case WS_EVT_CONNECT: return "connect";
    case WS_EVT_DISCONNECT: return "disconnect";
    case WS_EVT_DATA: return "data";
    case WS_EVT_PONG: return "pong";
    case WS_EVT_ERROR: return "error";
    default: return "other";
    }
}

const char* wsSendStatusName_(AsyncWebSocket::SendStatus status)
{
    switch (status) {
    case AsyncWebSocket::DISCARDED: return "drop";
    case AsyncWebSocket::ENQUEUED: return "q";
    case AsyncWebSocket::PARTIALLY_ENQUEUED: return "part";
    default: return "other";
    }
}

#if FLOW_WEB_HEAP_FORENSICS
void logWsEventHeapForensic_(const char* channel,
                             AwsEventType type,
                             AsyncWebSocketClient* client,
                             size_t len,
                             uint32_t clients,
                             uint32_t startUs,
                             const HeapForensicSnapshot& startHeap)
{
    const HeapForensicSnapshot endHeap = captureHeapForensicSnapshot_();
    const uint32_t elapsedUs = micros() - startUs;
    const long deltaFree = (long)endHeap.freeBytes - (long)startHeap.freeBytes;
    const uint32_t lowWaterDrop =
        (startHeap.minFreeBytes > endHeap.minFreeBytes) ? (startHeap.minFreeBytes - endHeap.minFreeBytes) : 0U;
    LOGW("WSfx %s %s id=%lu len=%u n=%u us=%lu f0=%lu f1=%lu df=%ld m1=%lu lo=%lu",
         channel ? channel : "?",
         wsEventName_(type),
         (unsigned long)(client ? client->id() : 0U),
         (unsigned)len,
         (unsigned)clients,
         (unsigned long)elapsedUs,
         (unsigned long)startHeap.freeBytes,
         (unsigned long)endHeap.freeBytes,
         deltaFree,
         (unsigned long)endHeap.minFreeBytes,
         (unsigned long)lowWaterDrop);
}

void logWsSendHeapForensic_(const char* channel,
                            const char* result,
                            size_t len,
                            uint32_t clients,
                            uint32_t startUs,
                            const HeapForensicSnapshot& startHeap)
{
    const HeapForensicSnapshot endHeap = captureHeapForensicSnapshot_();
    const uint32_t elapsedUs = micros() - startUs;
    const long deltaFree = (long)endHeap.freeBytes - (long)startHeap.freeBytes;
    const uint32_t lowWaterDrop =
        (startHeap.minFreeBytes > endHeap.minFreeBytes) ? (startHeap.minFreeBytes - endHeap.minFreeBytes) : 0U;
    LOGW("WSfx %s tx r=%s len=%u n=%u us=%lu f0=%lu f1=%lu df=%ld m1=%lu lo=%lu",
         channel ? channel : "?",
         result ? result : "?",
         (unsigned)len,
         (unsigned)clients,
         (unsigned long)elapsedUs,
         (unsigned long)startHeap.freeBytes,
         (unsigned long)endHeap.freeBytes,
         deltaFree,
         (unsigned long)endHeap.minFreeBytes,
         (unsigned long)lowWaterDrop);
}
#endif
} // namespace

void WebInterfaceModule::onWsLogEvent_(AsyncWebSocket*,
                                       AsyncWebSocketClient* client,
                                       AwsEventType type,
                                       void* arg,
                                       uint8_t* data,
                                       size_t len)
{
    if (type == WS_EVT_CONNECT ||
        type == WS_EVT_DISCONNECT ||
        type == WS_EVT_DATA ||
        type == WS_EVT_PONG ||
        type == WS_EVT_ERROR) {
        noteWsActivity_();
    }

#if FLOW_WEB_HEAP_FORENSICS
    const uint32_t forensicStartUs = micros();
    const HeapForensicSnapshot forensicStartHeap = captureHeapForensicSnapshot_();
#endif

    if (type == WS_EVT_CONNECT) {
        ++wsLogConnectCount_;
        if (client) {
            if (wsLog_.count() > 1U) {
                client->close(1008, "busy");
                LOGW("wslog reject client id=%lu clients=%u",
                     (unsigned long)client->id(),
                     (unsigned)wsLog_.count());
                return;
            }
            client->setCloseClientOnQueueFull(true);
            client->keepAlivePeriod(15);
            const bool flowSource = (wsActiveSource_() == 1U);
            client->text(flowSource
                ? "[webinterface] logs connectes source=flowio"
                : "[webinterface] logs connectes source=supervisor");
#if FLOW_ENABLE_BOOT_LOG_CAPTURE
            if (!bootLogCapture_) {
                bootLogCapture_ = bootLogCaptureService();
            }
            if (bootLogCapture_ && bootLogCapture_->getStats) {
                BootLogCaptureStats stats{};
                bootLogCapture_->getStats(bootLogCapture_->ctx, &stats);
                char bootStatus[128] = {0};
                snprintf(bootStatus,
                         sizeof(bootStatus),
                         "[webinterface] wslog connected; bootlog available entries=%u dropped=%lu state=%s",
                         (unsigned)stats.count,
                         (unsigned long)stats.droppedCount,
                         stats.capturing ? "capture" : (stats.complete ? "complete" : "idle"));
                client->text(bootStatus);
            }
#endif
        }
        LOGI("wslog connect id=%lu clients=%u connects=%lu",
             (unsigned long)(client ? client->id() : 0U),
             (unsigned)wsLog_.count(),
             (unsigned long)wsLogConnectCount_);
    } else if (type == WS_EVT_DISCONNECT) {
        ++wsLogDisconnectCount_;
        if (wsLog_.count() == 0U) {
            setWsActiveSource_(0U);
        }
        LOGW("wslog disconnect id=%lu clients=%u disconnects=%lu sent=%lu dropped=%lu partial=%lu discarded=%lu coalesced=%lu heap=%lu",
             (unsigned long)(client ? client->id() : 0U),
             (unsigned)wsLog_.count(),
             (unsigned long)wsLogDisconnectCount_,
             (unsigned long)wsLogSentCount_,
             (unsigned long)wsLogDropCount_,
             (unsigned long)wsLogPartialCount_,
             (unsigned long)wsLogDiscardCount_,
             (unsigned long)wsLogCoalescedCount_,
             (unsigned long)ESP.getFreeHeap());
    } else if (type == WS_EVT_DATA && arg && data && len > 0U) {
        AwsFrameInfo* info = reinterpret_cast<AwsFrameInfo*>(arg);
        if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT) {
            return;
        }
        char cmd[40] = {0};
        const size_t n = (len < (sizeof(cmd) - 1U)) ? len : (sizeof(cmd) - 1U);
        memcpy(cmd, data, n);
        cmd[n] = '\0';
        if (strcmp(cmd, "src:flowio") == 0) {
            if (bridgeUartEnabled_) {
                setWsActiveSource_(1U);
                if (client) client->text("[webinterface] source=flowio");
            } else {
                if (client) client->text("[webinterface] source=flowio indisponible");
            }
        } else if (strcmp(cmd, "src:supervisor") == 0) {
            setWsActiveSource_(0U);
            if (client) client->text("[webinterface] source=supervisor");
        } else if (strcmp(cmd, "bootlog:dump") == 0) {
            dumpBootLogCapture_(client);
        } else if (client) {
            client->text("[webinterface] cmd inconnu");
        }
    }

#if FLOW_WEB_HEAP_FORENSICS
    if (type == WS_EVT_CONNECT || type == WS_EVT_DISCONNECT || type == WS_EVT_ERROR) {
        logWsEventHeapForensic_("wslog",
                                type,
                                client,
                                0U,
                                (uint32_t)wsLog_.count(),
                                forensicStartUs,
                                forensicStartHeap);
    }
#endif
}

bool WebInterfaceModule::acquireRuntimeValuesBodyScratch_()
{
    bool acquired = false;
    portENTER_CRITICAL(&runtimeValuesBodyMux_);
    if (!runtimeValuesBodyBusy_) {
        runtimeValuesBodyBusy_ = true;
        acquired = true;
    }
    portEXIT_CRITICAL(&runtimeValuesBodyMux_);
    return acquired;
}

void WebInterfaceModule::releaseRuntimeValuesBodyScratch_()
{
    portENTER_CRITICAL(&runtimeValuesBodyMux_);
    runtimeValuesBodyBusy_ = false;
    portEXIT_CRITICAL(&runtimeValuesBodyMux_);
}

void WebInterfaceModule::flushLine_(bool force)
{
    if (lineLen_ == 0) return;
    if (!force) return;

    const size_t payloadLen = lineLen_;
    lineBuf_[lineLen_] = '\0';
    if (wsLog_.count() == 0) {
        lineLen_ = 0;
        return;
    }

    const HeapForensicSnapshot heapSnap = captureHeapForensicSnapshot_();
    if (heapSnap.freeBytes < kWsMinFreeBytes || heapSnap.largestFreeBlock < kWsMinLargestBytes) {
        ++wsFlowDropCount_;
        logWsFlowPressure_("heap_guard");
        lineLen_ = 0;
        return;
    }

#if FLOW_WEB_HEAP_FORENSICS
    const uint32_t forensicStartUs = micros();
    const HeapForensicSnapshot forensicStartHeap = captureHeapForensicSnapshot_();
#endif

    if (!wsLog_.availableForWriteAll()) {
        ++wsFlowDropCount_;
        logWsFlowPressure_("queue_full");
#if FLOW_WEB_HEAP_FORENSICS
        logWsSendHeapForensic_("wsflow",
                               "qfull",
                               payloadLen,
                               (uint32_t)wsLog_.count(),
                               forensicStartUs,
                               forensicStartHeap);
#endif
        lineLen_ = 0;
        return;
    }

    const AsyncWebSocket::SendStatus status = wsLog_.textAll(lineBuf_);
    if (status == AsyncWebSocket::ENQUEUED) {
        ++wsFlowSentCount_;
        noteWsActivity_();
    } else {
        ++wsFlowDropCount_;
        if (status == AsyncWebSocket::PARTIALLY_ENQUEUED) {
            ++wsFlowPartialCount_;
            logWsFlowPressure_("partial_enqueue");
        } else {
            ++wsFlowDiscardCount_;
            logWsFlowPressure_("discarded");
        }
    }
#if FLOW_WEB_HEAP_FORENSICS
    logWsSendHeapForensic_("wsflow",
                           wsSendStatusName_(status),
                           payloadLen,
                           (uint32_t)wsLog_.count(),
                           forensicStartUs,
                           forensicStartHeap);
#endif
    lineLen_ = 0;
}

void WebInterfaceModule::logWsFlowPressure_(const char* reason)
{
    const uint32_t nowMs = millis();
    if ((nowMs - wsFlowLastPressureLogMs_) < 2000U) return;
    wsFlowLastPressureLogMs_ = nowMs;
    LOGW("wsflow pressure reason=%s clients=%u sent=%lu dropped=%lu partial=%lu discarded=%lu heap=%lu",
         reason ? reason : "unknown",
         (unsigned)wsLog_.count(),
         (unsigned long)wsFlowSentCount_,
         (unsigned long)wsFlowDropCount_,
         (unsigned long)wsFlowPartialCount_,
         (unsigned long)wsFlowDiscardCount_,
         (unsigned long)ESP.getFreeHeap());
}

void WebInterfaceModule::logWsLogPressure_(const char* reason)
{
    const uint32_t nowMs = millis();
    if ((nowMs - wsLogLastPressureLogMs_) < 2000U) return;
    wsLogLastPressureLogMs_ = nowMs;
    LOGW("wslog pressure reason=%s clients=%u sent=%lu dropped=%lu partial=%lu discarded=%lu coalesced=%lu heap=%lu",
         reason ? reason : "unknown",
         (unsigned)wsLog_.count(),
         (unsigned long)wsLogSentCount_,
         (unsigned long)wsLogDropCount_,
         (unsigned long)wsLogPartialCount_,
         (unsigned long)wsLogDiscardCount_,
         (unsigned long)wsLogCoalescedCount_,
         (unsigned long)ESP.getFreeHeap());
}

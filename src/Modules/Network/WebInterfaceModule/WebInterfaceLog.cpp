/**
 * @file WebInterfaceLog.cpp
 * @brief Log formatting helpers and local sink fan-out for WebInterfaceModule.
 */

#include "WebInterfaceModule.h"

#include <time.h>
#include <esp_heap_caps.h>

namespace {
static constexpr uint32_t kWsLogMinFreeBytes = 12288U;
static constexpr uint32_t kWsLogMinLargestBytes = 3072U;
}

bool WebInterfaceModule::isLogByte_(uint8_t c)
{
    return c == '\t' || c == 0x1B || c >= 32;
}

char WebInterfaceModule::levelChar_(LogLevel lvl)
{
    if (lvl == LogLevel::Debug) return 'D';
    if (lvl == LogLevel::Info) return 'I';
    if (lvl == LogLevel::Warn) return 'W';
    if (lvl == LogLevel::Error) return 'E';
    return '?';
}

const char* WebInterfaceModule::levelColor_(LogLevel lvl)
{
    if (lvl == LogLevel::Debug) return "\x1b[90m";
    if (lvl == LogLevel::Info) return "\x1b[32m";
    if (lvl == LogLevel::Warn) return "\x1b[33m";
    if (lvl == LogLevel::Error) return "\x1b[31m";
    return "";
}

const char* WebInterfaceModule::colorReset_()
{
    return "\x1b[0m";
}

bool WebInterfaceModule::isSystemTimeValid_()
{
    time_t now = time(nullptr);
    return (now > 1609459200); // 2021-01-01 00:00:00
}

void WebInterfaceModule::formatUptime_(char* out, size_t outSize, uint32_t ms)
{
    uint32_t s = ms / 1000;
    uint32_t m = s / 60;
    uint32_t h = m / 60;
    const uint32_t hh = h % 24;
    const uint32_t mm = m % 60;
    const uint32_t ss = s % 60;
    const uint32_t mmm = ms % 1000;

    snprintf(out, outSize, "%02lu:%02lu:%02lu.%03lu",
             (unsigned long)hh,
             (unsigned long)mm,
             (unsigned long)ss,
             (unsigned long)mmm);
}

void WebInterfaceModule::formatTimestamp_(WebInterfaceModule* self, const LogEntry& e, char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';

    bool timeFromService = false;
    if (self) {
        if (!self->timeSvc_ && self->services_) {
            self->timeSvc_ = self->services_->get<TimeService>(ServiceId::Time);
        }
        if (self->timeSvc_ &&
            self->timeSvc_->isSynced &&
            self->timeSvc_->formatLocalTime &&
            self->timeSvc_->isSynced(self->timeSvc_->ctx)) {
            char localTs[32] = {0};
            if (self->timeSvc_->formatLocalTime(self->timeSvc_->ctx, localTs, sizeof(localTs))) {
                const unsigned ms = e.ts_ms % 1000;
                snprintf(out, outSize, "%s.%03u", localTs, ms);
                timeFromService = true;
            }
        }
    }

    if (!timeFromService && isSystemTimeValid_()) {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        const unsigned ms = e.ts_ms % 1000;
        snprintf(out, outSize,
                 "%04d-%02d-%02d %02d:%02d:%02d.%03u",
                 t.tm_year + 1900,
                 t.tm_mon + 1,
                 t.tm_mday,
                 t.tm_hour,
                 t.tm_min,
                 t.tm_sec,
                 ms);
        return;
    }

    if (!timeFromService) {
        formatUptime_(out, outSize, e.ts_ms);
    }
}

void WebInterfaceModule::formatLogEntryLine_(WebInterfaceModule* self,
                                             const LogEntry& e,
                                             char* out,
                                             size_t outSize,
                                             bool colorize)
{
    if (!out || outSize == 0U) return;
    out[0] = '\0';

    const char* moduleName = nullptr;
    char moduleFallback[24] = {0};
    if (self && self->logHub_ && self->logHub_->resolveModuleName) {
        moduleName = self->logHub_->resolveModuleName(self->logHub_->ctx, e.moduleId);
    }
    if (!moduleName || moduleName[0] == '\0') {
        snprintf(moduleFallback, sizeof(moduleFallback), "#%u", (unsigned)e.moduleId);
        moduleName = moduleFallback;
    }

    char ts[48] = {0};
    formatTimestamp_(self, e, ts, sizeof(ts));

    const char* color = colorize ? levelColor_(e.lvl) : "";
    const char* reset = colorize ? colorReset_() : "";
    int wrote = snprintf(out,
                         outSize,
                         "[%s][%c][%s] %s%s",
                         ts,
                         levelChar_(e.lvl),
                         moduleName,
                         color,
                         e.msg);
    if (wrote > 0 && (size_t)wrote < outSize) {
        wrote += snprintf(out + wrote, outSize - (size_t)wrote, "%s", reset);
    }
    if (wrote <= 0) {
        out[0] = '\0';
        return;
    }
    if ((size_t)wrote >= outSize) {
        out[outSize - 1U] = '\0';
    }
}

void WebInterfaceModule::onLocalLogSinkWrite_(void* ctx, const LogEntry& e)
{
    WebInterfaceModule* self = static_cast<WebInterfaceModule*>(ctx);
    if (!self || !self->localLogQueue_) return;

    char line[kLocalLogLineMax] = {0};
    formatLogEntryLine_(self, e, line, sizeof(line), true);
    if (line[0] == '\0') return;

    if (xQueueSend(self->localLogQueue_, line, 0) != pdTRUE) {
        ++self->wsLogDropCount_;
        ++self->wsLogCoalescedCount_;
        ++self->wsLogPendingSummaryDrops_;
        char dropped[kLocalLogLineMax] = {0};
        (void)xQueueReceive(self->localLogQueue_, dropped, 0);
        (void)xQueueSend(self->localLogQueue_, line, 0);
    }
}

void WebInterfaceModule::dumpBootLogCapture_(AsyncWebSocketClient* client)
{
    if (!client) return;
#if FLOW_ENABLE_BOOT_LOG_CAPTURE
    if (!bootLogCapture_) {
        bootLogCapture_ = bootLogCaptureService();
    }
#endif
    if (!bootLogCapture_ || !bootLogCapture_->getStats) {
        client->text("[webinterface] bootlog non disponible");
        return;
    }

    BootLogCaptureStats stats{};
    bootLogCapture_->getStats(bootLogCapture_->ctx, &stats);
    if (stats.capacity == 0 || !stats.psram) {
        client->text("[webinterface] bootlog non disponible: PSRAM inactive");
        return;
    }

    char header[128] = {0};
    snprintf(header,
             sizeof(header),
             "[webinterface] bootlog disponible entries=%u/%u dropped=%lu state=%s; charger via /api/logs/boot",
             (unsigned)stats.count,
             (unsigned)stats.capacity,
             (unsigned long)stats.droppedCount,
             stats.capturing ? "capture" : (stats.complete ? "complete" : "idle"));
    client->text(header);
}

void WebInterfaceModule::flushLocalLogQueue_()
{
    if (!started_ || !localLogQueue_) return;

    if (wsLog_.count() == 0U) {
        uint32_t drained = 0U;
        while (xQueueReceive(localLogQueue_, lineBuf_, 0) == pdTRUE) {
            ++drained;
        }
        if (drained > 0U) {
            wsLogDropCount_ += drained;
            wsLogCoalescedCount_ += drained;
            wsLogPendingSummaryDrops_ += drained;
        }
        return;
    }

    auto sendLogLine = [&](const char* text) -> bool {
        if (!text || text[0] == '\0') return true;
        const uint32_t freeBytes = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
        const uint32_t largestBytes = freeBytes;
        if (freeBytes < kWsLogMinFreeBytes || largestBytes < kWsLogMinLargestBytes) {
            ++wsLogDropCount_;
            ++wsLogCoalescedCount_;
            ++wsLogPendingSummaryDrops_;
            logWsLogPressure_("heap_guard");
            return false;
        }
        if (!wsLog_.availableForWriteAll()) {
            ++wsLogDropCount_;
            ++wsLogCoalescedCount_;
            ++wsLogPendingSummaryDrops_;
            logWsLogPressure_("queue_full");
            return false;
        }
        const AsyncWebSocket::SendStatus status = wsLog_.textAll(text);
        if (status == AsyncWebSocket::ENQUEUED) {
            ++wsLogSentCount_;
            noteWsActivity_();
            return true;
        }
        if (status == AsyncWebSocket::PARTIALLY_ENQUEUED) {
            noteWsActivity_();
        }
        ++wsLogDropCount_;
        ++wsLogCoalescedCount_;
        ++wsLogPendingSummaryDrops_;
        if (status == AsyncWebSocket::PARTIALLY_ENQUEUED) {
            ++wsLogPartialCount_;
            logWsLogPressure_("partial_enqueue");
        } else {
            ++wsLogDiscardCount_;
            logWsLogPressure_("discarded");
        }
        return false;
    };

    uint8_t sentBurst = 0U;
    while (sentBurst < kWsLogFlushBurstMax) {
        if (wsLogPendingSummaryDrops_ > 0U) {
            char summary[96] = {0};
            const uint32_t droppedCount = wsLogPendingSummaryDrops_;
            const int wrote = snprintf(summary,
                                       sizeof(summary),
                                       "[webinterface] logs coalesces: %lu ligne(s) ignoree(s)",
                                       (unsigned long)droppedCount);
            if (wrote <= 0 || !sendLogLine(summary)) {
                break;
            }
            wsLogPendingSummaryDrops_ = 0U;
            ++sentBurst;
            continue;
        }

        if (xQueueReceive(localLogQueue_, lineBuf_, 0) != pdTRUE) {
            break;
        }
        if (!sendLogLine(lineBuf_)) {
            uint32_t drained = 0U;
            while (xQueueReceive(localLogQueue_, lineBuf_, 0) == pdTRUE) {
                ++drained;
            }
            wsLogDropCount_ += drained;
            wsLogCoalescedCount_ += drained;
            wsLogPendingSummaryDrops_ += drained;
            break;
        }
        ++sentBurst;
    }
}

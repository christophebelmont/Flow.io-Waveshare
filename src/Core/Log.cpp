/**
 * @file Log.cpp
 * @brief Implementation file.
 */
#include "Core/Log.h"
#include "Core/LogModuleIds.h"
#include <stdarg.h>
#include <stdio.h>

namespace {
    const LogHubService* g_hub = nullptr;

    void logVaModule(LogLevel lvl, LogModuleId moduleId, const char* fmt, va_list ap) {
        if (!g_hub || !g_hub->enqueue || !fmt) return;
        if (g_hub->shouldLog && !g_hub->shouldLog(g_hub->ctx, moduleId, lvl)) return;

        LogEntry e{};
        e.ts_ms = millis();
        e.lvl = lvl;
        e.moduleId = moduleId;

        const int wrote = vsnprintf(e.msg, LOG_MSG_MAX, fmt, ap);
        if ((wrote < 0 || wrote >= LOG_MSG_MAX) && g_hub->noteFormatTruncation) {
            g_hub->noteFormatTruncation(g_hub->ctx, moduleId, (wrote < 0) ? 0U : (uint32_t)wrote);
        }
        g_hub->enqueue(g_hub->ctx, e);
    }
}

void Log::setHub(const LogHubService* hub) {
    g_hub = hub;
}

const LogHubService* Log::hub() {
    return g_hub;
}

bool Log::registerModule(LogModuleId moduleId, const char* moduleName) {
    if (!g_hub || !g_hub->registerModule) return false;
    return g_hub->registerModule(g_hub->ctx, moduleId, moduleName);
}

bool Log::setModuleMinLevel(LogModuleId moduleId, LogLevel level) {
    if (!g_hub || !g_hub->setModuleMinLevel) return false;
    return g_hub->setModuleMinLevel(g_hub->ctx, moduleId, level);
}

void Log::logf(LogLevel lvl, LogModuleId moduleId, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logVaModule(lvl, moduleId, fmt, ap);
    va_end(ap);
}

void Log::debug(LogModuleId moduleId, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logVaModule(LogLevel::Debug, moduleId, fmt, ap);
    va_end(ap);
}

void Log::info(LogModuleId moduleId, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logVaModule(LogLevel::Info, moduleId, fmt, ap);
    va_end(ap);
}

void Log::warn(LogModuleId moduleId, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logVaModule(LogLevel::Warn, moduleId, fmt, ap);
    va_end(ap);
}

void Log::error(LogModuleId moduleId, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logVaModule(LogLevel::Error, moduleId, fmt, ap);
    va_end(ap);
}

void Log::debugHex(LogModuleId moduleId, const char* label, uint32_t id,
                   const uint8_t* data, size_t length) {
    if (!g_hub || !g_hub->enqueue || !label || !data || length == 0U) return;
    if (g_hub->shouldLog && !g_hub->shouldLog(g_hub->ctx, moduleId, LogLevel::Debug)) return;

    // Leave room for the label, correlation id and byte offsets in LOG_MSG_MAX.
    constexpr size_t bytesPerLine = 24U;
    constexpr char digits[] = "0123456789ABCDEF";
    char hex[bytesPerLine * 3U];
    for (size_t offset = 0U; offset < length;) {
        const size_t count = length - offset < bytesPerLine ? length - offset : bytesPerLine;
        for (size_t i = 0U; i < count; ++i) {
            const uint8_t byte = data[offset + i];
            hex[i * 3U] = digits[byte >> 4U];
            hex[i * 3U + 1U] = digits[byte & 0x0FU];
            hex[i * 3U + 2U] = i + 1U == count ? '\0' : ' ';
        }
        debug(moduleId, "%s id=%lu offset=%u bytes=%u/%u hex=%s", label,
              (unsigned long)id, (unsigned)offset, (unsigned)count, (unsigned)length, hex);
        offset += count;
    }
}

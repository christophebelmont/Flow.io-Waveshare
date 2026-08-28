/**
 * @file LogSerialSinkModule.cpp
 * @brief Implementation file.
 */
#include "LogSerialSinkModule.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Board/BoardSerialMap.h"
#include "Core/SnprintfCheck.h"

#undef snprintf
#define snprintf(OUT, LEN, FMT, ...) \
    FLOW_SNPRINTF_CHECKED_MODULE((LogModuleId)LogModuleIdValue::LogSinkSerial, OUT, LEN, FMT, ##__VA_ARGS__)

struct SerialSinkCtx {
    ServiceRegistry* services = nullptr;
    const TimeService* timeSvc = nullptr;
    const LogHubService* hubSvc = nullptr;
};

static SerialSinkCtx gSerialSinkCtx{};
static Stream* gLogSerial = &Serial;
static SemaphoreHandle_t gSerialWriteMutex = nullptr;

static const char* lvlStr(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Debug: return "D";
        case LogLevel::Info:  return "I";
        case LogLevel::Warn:  return "W";
        case LogLevel::Error: return "E";
    }
    return "?";
}

static const char* lvlColor(LogLevel lvl) {
#if defined(FLOW_LOG_SERIAL_COLORS) && (FLOW_LOG_SERIAL_COLORS == 1)
    switch (lvl) {
        case LogLevel::Debug: return "\x1b[90m";
        case LogLevel::Info:  return "\x1b[32m";
        case LogLevel::Warn:  return "\x1b[33m";
        case LogLevel::Error: return "\x1b[31m";
    }
#endif
    (void)lvl;
    return "";
}

static const char* colorReset() {
#if defined(FLOW_LOG_SERIAL_COLORS) && (FLOW_LOG_SERIAL_COLORS == 1)
    return "\x1b[0m";
#else
    return "";
#endif
}

static bool isSystemTimeValid()
{
    // Classic ESP32 behavior: before real sync, epoch is near 1970.
    time_t now = time(nullptr);
    return (now > 1609459200); ///< 2021-01-01 00:00:00
}

static void formatUptime(char *out, size_t outSize, uint32_t ms)
{
    uint32_t s   = ms / 1000;
    uint32_t m   = s / 60;
    uint32_t h   = m / 60;

    uint32_t hh  = h % 24;
    uint32_t mm  = m % 60;
    uint32_t ss  = s % 60;
    uint32_t mmm = ms % 1000;

    snprintf(out, outSize, "%02lu:%02lu:%02lu.%03lu",
             (unsigned long)hh,
             (unsigned long)mm,
             (unsigned long)ss,
             (unsigned long)mmm);
}

static void serialSinkWrite(void* ctx, const LogEntry& e) {
    SerialSinkCtx* sinkCtx = static_cast<SerialSinkCtx*>(ctx);
    const char* moduleName = nullptr;
    char moduleFallback[24] = {0};

    if (sinkCtx) {
        if (!sinkCtx->hubSvc && sinkCtx->services) {
            sinkCtx->hubSvc = sinkCtx->services->get<LogHubService>(ServiceId::LogHub);
        }
        if (sinkCtx->hubSvc && sinkCtx->hubSvc->resolveModuleName) {
            moduleName = sinkCtx->hubSvc->resolveModuleName(sinkCtx->hubSvc->ctx, e.moduleId);
        }
    }
    if (!moduleName || moduleName[0] == '\0') {
        snprintf(moduleFallback, sizeof(moduleFallback), "#%u", (unsigned)e.moduleId);
        moduleName = moduleFallback;
    }

    char ts[48];
    bool timeFromService = false;

    if (sinkCtx) {
        if (!sinkCtx->timeSvc && sinkCtx->services) {
            sinkCtx->timeSvc = sinkCtx->services->get<TimeService>(ServiceId::Time);
        }

        if (sinkCtx->timeSvc &&
            sinkCtx->timeSvc->isSynced &&
            sinkCtx->timeSvc->formatLocalTime &&
            sinkCtx->timeSvc->isSynced(sinkCtx->timeSvc->ctx)) {
            char localTs[32] = {0};
            if (sinkCtx->timeSvc->formatLocalTime(sinkCtx->timeSvc->ctx, localTs, sizeof(localTs))) {
                unsigned ms = e.ts_ms % 1000;
                snprintf(ts, sizeof(ts), "%s.%03u", localTs, ms);
                timeFromService = true;
            }
        }
    }

    if (!timeFromService && isSystemTimeValid()) {
        // Real system time (NTP synced)
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);

        unsigned ms = e.ts_ms % 1000;

        snprintf(ts, sizeof(ts),
                 "%04d-%02d-%02d %02d:%02d:%02d.%03u",
                 t.tm_year + 1900,
                 t.tm_mon + 1,
                 t.tm_mday,
                 t.tm_hour,
                 t.tm_min,
                 t.tm_sec,
                 ms);
    } else if (!timeFromService) {
        // Fallback uptime
        formatUptime(ts, sizeof(ts), e.ts_ms);
    }

    if (!gLogSerial) return;

    const char* color = lvlColor(e.lvl);
    char line[(size_t)LOG_MSG_MAX + 96U] = {0};
    const int wrote = snprintf(line,
                               sizeof(line),
                               "[%s][%s][%s] %s%s%s\r\n",
                               ts,
                               lvlStr(e.lvl),
                               moduleName,
                               color,
                               e.msg,
                               colorReset());
    if (wrote <= 0) return;
    size_t len = (size_t)wrote;
    if (len >= sizeof(line)) len = sizeof(line) - 1U;

    if (gSerialWriteMutex) {
        if (xSemaphoreTake(gSerialWriteMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            (void)gLogSerial->write((const uint8_t*)line, len);
            xSemaphoreGive(gSerialWriteMutex);
            return;
        }
    }

    (void)gLogSerial->write((const uint8_t*)line, len);
}

void LogSerialSinkModule::init(ConfigStore& cfg, ServiceRegistry& services) {
    (void)cfg;

    gLogSerial = &Board::SerialMap::logSerial();
    Board::SerialMap::beginLogSerial();
    if (!gSerialWriteMutex) {
        gSerialWriteMutex = xSemaphoreCreateMutex();
    }

    auto sinks = services.get<LogSinkRegistryService>(ServiceId::LogSinks);
    if (!sinks) return;

    gSerialSinkCtx.services = &services;
    gSerialSinkCtx.timeSvc = nullptr;
    gSerialSinkCtx.hubSvc = services.get<LogHubService>(ServiceId::LogHub);

    LogSinkService sink{};
    sink.write = serialSinkWrite;
    sink.ctx = &gSerialSinkCtx;

    sinks->add(sinks->ctx, sink);
}

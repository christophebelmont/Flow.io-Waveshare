/**
 * @file Log.h
 * @brief Global log helper for core and modules.
 */
#pragma once

#include "Core/ModuleId.h"
#include "Core/Services/ILogger.h"

namespace Log {
    /**
     * @brief Set the global log hub service.
     */
    void setHub(const LogHubService* hub);

    /**
     * @brief Get the current global log hub service.
     */
    const LogHubService* hub();

    /**
     * @brief Register or update a module name for a numeric module id.
     */
    bool registerModule(LogModuleId moduleId, const char* moduleName);

    /**
     * @brief Set the minimum level emitted for a module id.
     */
    bool setModuleMinLevel(LogModuleId moduleId, LogLevel level);

    /**
     * @brief Log a formatted message with a given level.
     */
    void logf(LogLevel lvl, LogModuleId moduleId, const char* fmt, ...);

    /** @brief Convenience: Debug log. */
    void debug(LogModuleId moduleId, const char* fmt, ...);
    /**
     * @brief Dump bytes at Debug level, in bounded lines with a correlation id.
     * No formatting is performed when Debug is disabled for the module.
     */
    void debugHex(LogModuleId moduleId, const char* label, uint32_t id,
                  const uint8_t* data, size_t length);
    /** @brief Convenience: Info log. */
    void info(LogModuleId moduleId, const char* fmt, ...);
    /** @brief Convenience: Warning log. */
    void warn(LogModuleId moduleId, const char* fmt, ...);
    /** @brief Convenience: Error log. */
    void error(LogModuleId moduleId, const char* fmt, ...);
}

// Macros are provided by Core/ModuleLog.h to keep Module.h neutral.

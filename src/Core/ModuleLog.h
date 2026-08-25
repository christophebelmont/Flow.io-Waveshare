/**
 * @file ModuleLog.h
 * @brief Convenience logging macros for modules.
 */
#pragma once

#include "Core/Log.h"
#include "Core/LogModuleIds.h"
#include "Core/SnprintfCheck.h"

// Ensure a default module id if not defined by the module.
#ifndef LOG_MODULE_ID
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::Unknown)
#endif

#undef LOGD
#undef LOGI
#undef LOGW
#undef LOGE

#define LOGD(...) ::Log::debug((LogModuleId)(LOG_MODULE_ID), __VA_ARGS__)
#define LOGI(...) ::Log::info((LogModuleId)(LOG_MODULE_ID), __VA_ARGS__)
#define LOGW(...) ::Log::warn((LogModuleId)(LOG_MODULE_ID), __VA_ARGS__)
#define LOGE(...) ::Log::error((LogModuleId)(LOG_MODULE_ID), __VA_ARGS__)

#ifndef FLOW_SNPRINTF_WRAP_ACTIVE
#define FLOW_SNPRINTF_WRAP_ACTIVE 1
#undef snprintf
#define snprintf(OUT, LEN, FMT, ...) \
    FLOW_SNPRINTF_CHECKED_MODULE((LogModuleId)(LOG_MODULE_ID), OUT, LEN, FMT, ##__VA_ARGS__)
#endif

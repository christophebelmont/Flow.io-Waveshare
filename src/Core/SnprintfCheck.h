#pragma once
/**
 * @file SnprintfCheck.h
 * @brief Checked snprintf helper that logs truncation with source location.
 */

#include "Core/Log.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static inline int flowSnprintfCheckedModule_(LogModuleId moduleId,
                                             const char* file,
                                             int line,
                                             char* out,
                                             size_t outLen,
                                             const char* fmt,
                                             ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int wrote = vsnprintf(out, outLen, fmt, ap);
    va_end(ap);

    const bool truncated = (wrote < 0) || (outLen == 0) || ((size_t)wrote >= outLen);
    if (truncated) {
        Log::warn(moduleId,
                  "snprintf truncated at %s:%d (len=%u wrote=%d)",
                  file ? file : "?",
                  line,
                  (unsigned)outLen,
                  wrote);
    }
    return wrote;
}

#define FLOW_SNPRINTF_CHECKED_MODULE(MODULE_ID, OUT, LEN, FMT, ...) \
    flowSnprintfCheckedModule_((MODULE_ID), __FILE__, __LINE__, (OUT), (LEN), (FMT), ##__VA_ARGS__)

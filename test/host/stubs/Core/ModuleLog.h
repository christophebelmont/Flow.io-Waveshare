#pragma once
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)

// Keep arguments type-checked and used, as with the production logging call.
inline void hostLogDebug(const char*, ...) __attribute__((format(printf, 1, 2)));
inline void hostLogDebug(const char*, ...) {}
#define LOGD(...) hostLogDebug(__VA_ARGS__)

#include "Core/Log.h"
#define LOGD_HEX(LABEL, ID, DATA, LENGTH) \
    ::Log::debugHex((LogModuleId)(LOG_MODULE_ID), LABEL, ID, DATA, LENGTH)

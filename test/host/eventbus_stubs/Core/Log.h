#pragma once
#include "Core/LogModuleIds.h"
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
inline std::vector<std::string> eventbusWarnings;
namespace Log {
inline void debug(LogModuleId, const char*, ...) {}
inline void warn(LogModuleId, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    eventbusWarnings.emplace_back(text);
}
}

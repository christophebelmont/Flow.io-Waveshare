#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

/** Add a typed target to the compact command arguments before dispatch. */
inline bool appendRuntimeActionTarget(char* args,
                                     size_t capacity,
                                     const char* targetName,
                                     uint32_t target)
{
    if (!args || !capacity || !targetName || !targetName[0]) return false;

    StaticJsonDocument<192> doc;
    // A const input makes ArduinoJson copy the parsed strings into its pool.
    // A mutable input would keep pointers into args and corrupt the JSON when
    // serialization writes back into the same buffer.
    const char* input = args;
    if (deserializeJson(doc, input) || !doc.is<JsonObject>()) return false;
    doc[targetName] = target;
    if (doc.overflowed() || measureJson(doc) >= capacity) return false;
    serializeJson(doc, args, capacity);
    return true;
}

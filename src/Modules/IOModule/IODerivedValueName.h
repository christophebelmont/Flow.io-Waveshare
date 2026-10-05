#pragma once
/**
 * @file IODerivedValueName.h
 * @brief Resolve the configured display name of an IO derived value.
 */

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "Core/ConfigStore.h"

/** Read `io/value/vNN/name`; returns false when absent or empty. */
inline bool ioReadDerivedValueName(ConfigStore* cfgStore, uint8_t slot, char* out, size_t outLen)
{
    if (!out || outLen == 0U) return false;
    out[0] = '\0';
    if (!cfgStore) return false;

    char module[24] = {0};
    snprintf(module, sizeof(module), "io/value/v%02u", (unsigned)slot);
    char json[512] = {0};
    bool truncated = false;
    if (!cfgStore->toJsonModule(module, json, sizeof(json), &truncated, true) || truncated) return false;

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, json)) return false;
    const char* name = doc["name"] | "";
    if (name[0] == '\0') return false;
    snprintf(out, outLen, "%s", name);
    return true;
}

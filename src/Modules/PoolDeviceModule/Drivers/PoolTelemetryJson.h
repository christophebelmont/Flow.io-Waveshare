#pragma once
#include <ArduinoJson.h>
#include "Core/Services/PoolSerialTelemetry.h"

inline void writePoolTelemetryJson(JsonObject root, PoolTelemetryProfile profile, const PoolTelemetryState& state,
                                   bool includeUnits = false)
{
    if (profile == PoolTelemetryProfile::None) return;
    auto values = root.createNestedObject("telemetry");
    for (const auto& field : kHeatPumpPolyFields) {
        if (!(state.validBlocks & (1U << field.block))) values[field.key] = nullptr;
        else if (field.mask) values[field.key] = poolTelemetryValue(state, field) != 0;
        else values[field.key] = poolTelemetryValue(state, field);
    }
    for (uint16_t reg = 501; reg <= 503; ++reg) {
        const uint8_t block = 0;
        const char* key = reg == 501 ? "alarm_501" : reg == 502 ? "alarm_502" : "alarm_503";
        if (!(state.validBlocks & (1U << block))) values[key] = nullptr;
        else values[key] = state.raw[reg - 500];
    }
    for (const auto& alarm : kHeatPumpPolyAlarmFields) {
        if (!(state.validBlocks & 1U)) values[alarm.key] = nullptr;
        else values[alarm.key] = (state.raw[alarm.index] & alarm.mask) != 0;
    }
    root["telemetry_error"] = state.error;
    if (includeUnits) {
        auto units = root.createNestedObject("telemetry_units");
        for (const auto& field : kHeatPumpPolyFields) if (field.unit[0]) units[field.key] = field.unit;
    }
}

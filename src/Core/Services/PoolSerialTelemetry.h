#pragma once
#include <stdint.h>
#include <stddef.h>

enum class PoolTelemetryProfile : uint8_t { None, HeatPumpPoly };
constexpr uint8_t POOL_TELEMETRY_BLOCKS = 5;
constexpr uint8_t POOL_TELEMETRY_REGISTERS = 14;

struct PoolTelemetryState {
    uint16_t raw[POOL_TELEMETRY_REGISTERS]{};
    uint32_t readAtMs[POOL_TELEMETRY_BLOCKS]{};
    uint8_t validBlocks = 0;
    uint8_t error = 0;
};
struct PoolTelemetryBlock {
    uint16_t address;
    uint8_t count;
    uint8_t offset;
};
struct PoolTelemetryField {
    const char* key;
    const char* label;
    const char* unit;
    uint8_t block;
    uint8_t index;
    uint16_t mask; // Nonzero selects a boolean flag; zero selects a numeric register.
    bool signedValue;
    float scale;
};

/** Register map from Polytropic external gateway specification v6. */
inline constexpr PoolTelemetryBlock kHeatPumpPolyBlocks[] = {
    {500, 1, 0}, {503, 1, 1}, {510, 7, 2}, {521, 3, 9}, {1000, 2, 12}
};
inline constexpr PoolTelemetryField kHeatPumpPolyFields[] = {
    {"high_pressure", "High pressure", "", 0, 0, 0x0001, false, 1},
    {"low_pressure", "Low pressure", "", 0, 0, 0x0002, false, 1},
    {"water_flow", "Water flow", "", 0, 0, 0x0008, false, 1},
    {"water_pump", "Water pump", "", 0, 0, 0x0020, false, 1},
    {"electric_heater", "Electric heater", "", 0, 0, 0x0040, false, 1},
    {"four_way_valve", "Four-way valve", "", 0, 0, 0x0080, false, 1},
    {"bottom_plate", "Bottom plate", "", 0, 0, 0x0100, false, 1},
    {"compressor_heater", "Compressor heater", "", 0, 0, 0x0200, false, 1},
    {"defrost", "Defrost", "", 1, 1, 0x8000, false, 1},
    {"compressor_out", "Compressor outlet", "°C", 2, 2, 0, true, .1f},
    {"compressor_in", "Compressor inlet", "°C", 2, 3, 0, true, .1f},
    {"water_in", "Water inlet", "°C", 2, 4, 0, true, .1f},
    {"water_out", "Water outlet", "°C", 2, 5, 0, true, .1f},
    {"coil", "Coil", "°C", 2, 6, 0, true, .1f},
    {"ambient", "Ambient", "°C", 2, 7, 0, true, .1f},
    {"ipm", "IPM", "°C", 2, 8, 0, true, .1f},
    {"voltage", "Voltage", "V", 3, 9, 0, false, 1},
    {"current", "Current", "A", 3, 10, 0, false, .1f},
    {"compressor_fault", "Compressor fault", "", 3, 11, 0, false, 1},
    {"control_word", "Control word", "", 4, 12, 0, false, 1},
    {"temperature_setpoint", "Temperature setpoint", "°C", 4, 13, 0, true, .1f}
};
inline constexpr size_t POOL_TELEMETRY_FIELDS = sizeof(kHeatPumpPolyFields) / sizeof(kHeatPumpPolyFields[0]);

inline int32_t poolRegisterNumber(uint16_t raw, bool signedValue)
{
    return signedValue && raw >= 0x8000U ? int32_t(raw) - 65536 : int32_t(raw);
}
inline float poolTelemetryValue(const PoolTelemetryState& state, const PoolTelemetryField& field)
{
    const uint16_t raw = state.raw[field.index];
    return field.mask ? float((raw & field.mask) != 0) : poolRegisterNumber(raw, field.signedValue) * field.scale;
}

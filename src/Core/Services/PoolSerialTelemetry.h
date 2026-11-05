#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Core/Generated/EquipmentMetric_Generated.h"

enum class PoolTelemetryProfile : uint8_t { None, HeatPumpPoly };
constexpr uint8_t POOL_TELEMETRY_BLOCKS = 2;
constexpr uint8_t POOL_TELEMETRY_REGISTERS = 26;

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
    EquipmentMetric metric;
};

/** Register map from Polytropic external gateway specification v6. */
inline constexpr PoolTelemetryBlock kHeatPumpPolyBlocks[] = {
    {500, 24, 0}, {1000, 2, 24}
};
static_assert(sizeof(kHeatPumpPolyBlocks) / sizeof(kHeatPumpPolyBlocks[0]) == POOL_TELEMETRY_BLOCKS);
static_assert(kHeatPumpPolyBlocks[0].offset + kHeatPumpPolyBlocks[0].count <= POOL_TELEMETRY_REGISTERS);
static_assert(kHeatPumpPolyBlocks[1].offset + kHeatPumpPolyBlocks[1].count <= POOL_TELEMETRY_REGISTERS);
// Register 501 bit 14 (FLO). Retain an asserted alarm until a successful
// read clears it, including while the telemetry block is stale.
inline bool poolFlowProtectionActive(const PoolTelemetryState& telemetry)
{
    return (telemetry.raw[1] & 0x4000U) != 0;
}
inline constexpr PoolTelemetryField kHeatPumpPolyFields[] = {
    {"high_pressure", "High pressure", "", 0, 0, 0x0001, false, 1, EquipmentMetric::HighPressure},
    {"low_pressure", "Low pressure", "", 0, 0, 0x0002, false, 1, EquipmentMetric::LowPressure},
    {"water_flow", "Water flow", "", 0, 0, 0x0008, false, 1, EquipmentMetric::WaterFlow},
    {"water_pump", "Water pump", "", 0, 0, 0x0020, false, 1, EquipmentMetric::WaterPump},
    {"electric_heater", "Electric heater", "", 0, 0, 0x0040, false, 1, EquipmentMetric::ElectricHeater},
    {"four_way_valve", "Four-way valve", "", 0, 0, 0x0080, false, 1, EquipmentMetric::FourWayValve},
    {"bottom_plate", "Bottom plate", "", 0, 0, 0x0100, false, 1, EquipmentMetric::BottomPlate},
    {"compressor_heater", "Compressor heater", "", 0, 0, 0x0200, false, 1, EquipmentMetric::CompressorHeater},
    {"defrost", "Defrost", "", 0, 3, 0x8000, false, 1, EquipmentMetric::Defrost},
    {"compressor_out", "Compressor outlet", "°C", 0, 10, 0, true, .1f, EquipmentMetric::CompressorOut},
    {"compressor_in", "Compressor inlet", "°C", 0, 11, 0, true, .1f, EquipmentMetric::CompressorIn},
    {"water_in", "Water inlet", "°C", 0, 12, 0, true, .1f, EquipmentMetric::WaterIn},
    {"water_out", "Water outlet", "°C", 0, 13, 0, true, .1f, EquipmentMetric::WaterOut},
    {"coil", "Coil", "°C", 0, 14, 0, true, .1f, EquipmentMetric::Coil},
    {"ambient", "Ambient", "°C", 0, 15, 0, true, .1f, EquipmentMetric::Ambient},
    {"ipm", "IPM", "°C", 0, 16, 0, true, .1f, EquipmentMetric::Ipm},
    {"voltage", "Voltage", "V", 0, 21, 0, false, 1, EquipmentMetric::Voltage},
    {"current", "Current", "A", 0, 22, 0, false, .1f, EquipmentMetric::Current},
    {"compressor_fault", "Compressor fault", "", 0, 23, 0, false, 1, EquipmentMetric::CompressorFault},
    {"control_word", "Control word", "", 1, 24, 0, false, 1, EquipmentMetric::ControlWord},
    {"temperature_setpoint", "Temperature setpoint", "°C", 1, 25, 0, true, .1f, EquipmentMetric::TemperatureSetpoint},
    {"fan_1_speed", "Fan 1 speed (raw)", "", 0, 6, 0, false, 1, EquipmentMetric::Fan1Speed},
    {"fan_2_speed", "Fan 2 speed (raw)", "", 0, 7, 0, false, 1, EquipmentMetric::Fan2Speed},
    {"fan_1_level", "Fan 1 level", "", 0, 8, 0, false, 1, EquipmentMetric::Fan1Level},
    {"fan_2_level", "Fan 2 level", "", 0, 9, 0, false, 1, EquipmentMetric::Fan2Level},
    {"compressor_target_frequency", "Compressor target frequency", "Hz", 0, 17, 0, false, 1, EquipmentMetric::CompressorTargetFrequency},
    {"compressor_frequency", "Compressor frequency", "Hz", 0, 18, 0, false, 1, EquipmentMetric::CompressorFrequency},
    {"compressor_run_time", "Compressor run time", "min", 0, 19, 0, false, 1, EquipmentMetric::CompressorRunTime},
    {"compressor_stop_time", "Compressor stop time", "min", 0, 20, 0, false, 1, EquipmentMetric::CompressorStopTime},
    {"eev_1_opening", "EEV 1 opening", "", 0, 4, 0, false, 1, EquipmentMetric::Eev1Opening},
    {"eev_2_opening", "EEV 2 opening", "", 0, 5, 0, false, 1, EquipmentMetric::Eev2Opening}
};
inline constexpr size_t POOL_TELEMETRY_FIELDS = sizeof(kHeatPumpPolyFields) / sizeof(kHeatPumpPolyFields[0]);

struct PoolTelemetryAlarmField {
    const char* key;
    uint8_t index;
    uint16_t mask;
};
inline constexpr PoolTelemetryAlarmField kHeatPumpPolyAlarmFields[] = {
    {"fault_water_inlet", 1, 0x0001}, {"fault_water_outlet", 1, 0x0002},
    {"fault_coil_sensor", 1, 0x0004}, {"fault_ambient_sensor", 1, 0x0008},
    {"protection_ambient_low", 1, 0x0010}, {"fault_discharge_sensor", 1, 0x0020},
    {"fault_suction_sensor", 1, 0x0040}, {"protection_delta_t_high", 1, 0x0080},
    {"protection_delta_t_high_3x", 1, 0x0100}, {"protection_overcool", 1, 0x0200},
    {"protection_high_pressure", 1, 0x0400}, {"protection_high_pressure_3x", 1, 0x0800},
    {"protection_low_pressure", 1, 0x1000}, {"protection_low_pressure_3x", 1, 0x2000},
    {"protection_water_flow", 1, 0x4000}, {"protection_antifreeze", 1, 0x8000},
    {"fault_discharge_temperature_high", 2, 0x0001}, {"fault_ac_voltage", 2, 0x0002},
    {"fault_dc_current", 2, 0x0004}, {"fault_ac_current", 2, 0x0008},
    {"fault_compressor_overcurrent", 2, 0x0010}, {"fault_ipm_overheat", 2, 0x0020},
    {"fault_ipm", 2, 0x0040}, {"fault_compressor_driver", 2, 0x0080},
    {"fault_fan_1", 2, 0x0100}, {"fault_driver_communication", 2, 0x0400},
    {"fault_main_pcb_communication", 3, 0x0001}, {"fault_eeprom", 3, 0x0002}
};
inline constexpr size_t POOL_TELEMETRY_ALARMS = sizeof(kHeatPumpPolyAlarmFields) / sizeof(kHeatPumpPolyAlarmFields[0]);

inline int32_t poolRegisterNumber(uint16_t raw, bool signedValue)
{
    return signedValue && raw >= 0x8000U ? int32_t(raw) - 65536 : int32_t(raw);
}
inline float poolTelemetryValue(const PoolTelemetryState& state, const PoolTelemetryField& field)
{
    const uint16_t raw = state.raw[field.index];
    return field.mask ? float((raw & field.mask) != 0) : poolRegisterNumber(raw, field.signedValue) * field.scale;
}

#pragma once
#include <stdint.h>
// Generated stable metric identities; never renumber existing entries.
enum class EquipmentMetric : uint8_t {
    Requested = 0,
    Observed = 1,
    Running = 2,
    HighPressure = 8,
    LowPressure = 9,
    WaterFlow = 10,
    WaterPump = 11,
    ElectricHeater = 12,
    FourWayValve = 13,
    BottomPlate = 14,
    CompressorHeater = 15,
    Defrost = 16,
    CompressorOut = 17,
    CompressorIn = 18,
    WaterIn = 19,
    WaterOut = 20,
    Coil = 21,
    Ambient = 22,
    Ipm = 23,
    Voltage = 24,
    Current = 25,
    CompressorFault = 26,
    ControlWord = 27,
    TemperatureSetpoint = 28,
    Fan1Speed = 29,
    Fan2Speed = 30,
    Fan1Level = 31,
    Fan2Level = 32,
    CompressorTargetFrequency = 33,
    CompressorFrequency = 34,
    CompressorRunTime = 35,
    CompressorStopTime = 36,
    Eev1Opening = 37,
    Eev2Opening = 38,
};
inline constexpr uint16_t EquipmentValueBase = 176;
inline constexpr uint16_t EquipmentValueStride = 40;
inline constexpr uint16_t EquipmentValueLegacyStride = 32;
inline constexpr uint16_t EquipmentValueSlots = 16;

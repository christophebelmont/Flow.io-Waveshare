#pragma once

#include "Domain/DomainTypes.h"

namespace PoolIds {

enum DomainSlot : DomainSlotId {
    SensorOrp = 1,
    SensorPh = 2,
    SensorPsi = 3,
    SensorSpareAnalog = 4,
    SensorWaterTemp = 5,
    SensorAirTemp = 6,
    SensorCurrent = 7,
    SensorVoltage = 8,
    SensorPir = 9,
    SensorPhLevel = 10,
    SensorChlorineLevel = 11,
    SensorPoolLevel = 12,
    SensorWaterMeter = 13,
    ActuatorFiltrationPump = 14,
    ActuatorPhPump = 15,
    ActuatorChlorinePump = 16,
    ActuatorRobot = 17,
    ActuatorFillPump = 18,
    ActuatorChlorineGenerator = 19,
    ActuatorWaterHeater = 20,
    SensorWaterCounter = 21, // Kept outside the active Waveshare domain so shared modules still compile.
    ActuatorLights = 22
};

enum Device : PoolDeviceId {
    DeviceFiltrationPump = 0,
    DevicePhPump = 1,
    DeviceChlorinePump = 2,
    DeviceRobot = 3,
    DeviceFillPump = 4,
    DeviceChlorineGenerator = 5,
    DeviceLights = 6,
    DeviceWaterHeater = 7
};

constexpr uint8_t DeviceCount = 8;
constexpr uint8_t SensorCount = 13;
constexpr uint8_t DomainSlotCount = 21;

}  // namespace PoolIds

namespace PoolInputSlots {

// Canonical logical input slots for the pool installation.
// GPIO4 remains owned by the system reset handler; it is represented by i00
// but is not exposed as a configurable IOModule binding.
constexpr uint8_t FactoryReset = 0;
constexpr uint8_t WaterMeter = 1;
constexpr uint8_t PoolLevel = 2;
constexpr uint8_t ChlorineLevel = 3;
constexpr uint8_t PhLevel = 4;
constexpr uint8_t FlowMeter = 5;
constexpr uint8_t Spare = 6;
constexpr uint8_t Pir = 7;

}  // namespace PoolInputSlots

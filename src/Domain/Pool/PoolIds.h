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

#pragma once

#include "Domain/Pool/PoolIds.h"

/** PoolLogic's configured equipment roles, independent of dashboard visibility. */
struct PoolManualDeviceSlots {
    uint8_t filtration;
    uint8_t phPump;
    uint8_t disinfectionPump;
    uint8_t robot;
    uint8_t heater;
    uint8_t chlorineGenerator;
};

/** Resolve a manual write to the same business command used by equipment tiles. */
inline const char* poolManualDeviceWriteCommand(uint8_t slot, const PoolManualDeviceSlots& roles)
{
    if (slot == roles.filtration) return "poollogic.filtration.write";
    if (slot == roles.phPump) return "poollogic.ph_pump.write";
    if (slot == roles.disinfectionPump) return "poollogic.dis_pump.write";
    if (slot == roles.robot) return "poollogic.robot.write";
    if (slot == roles.heater) return "poollogic.heater.write";
    if (slot == roles.chlorineGenerator) return "poollogic.chlorine_generator.write";
    if (slot == PoolIds::DeviceLights) return "poollogic.lights.write";
    return "pooldevice.write";
}

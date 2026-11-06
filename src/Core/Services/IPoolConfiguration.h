#pragma once
/**
 * @file IPoolConfiguration.h
 * @brief Pool characteristics and authoritative actuator settings shared with history and insight modules.
 */

#include <stdint.h>

enum class PoolDisinfectionMethod : uint8_t {
    ChlorineBromine = 0,
    SaltElectrolysis,
    ActiveOxygen,
    Disabled,
};

constexpr const char* poolDisinfectionMethodName(PoolDisinfectionMethod method)
{
    switch (method) {
        case PoolDisinfectionMethod::Disabled: return "Disabled";
        case PoolDisinfectionMethod::ChlorineBromine: return "Chlorine/Bromine";
        case PoolDisinfectionMethod::SaltElectrolysis: return "Salt Electrolysis";
        case PoolDisinfectionMethod::ActiveOxygen: return "Active Oxygen";
    }
    return "Disabled";
}

struct PoolCharacteristics {
    bool available = false;
    bool volumeValid = false;
    float volumeM3 = 0.0f;
    bool indoor = false;
    bool automaticCoverPresent = false;
    bool coverClosedAtNight = false;
    PoolDisinfectionMethod disinfectionMethod = PoolDisinfectionMethod::Disabled;
};

/** @brief Current typed regulation settings exposed by PoolLogic. */
struct PoolOperatingConfiguration {
    bool available = false;
    bool filtrationAutoMode = false;
    bool phAutoMode = false;
    bool orpAutoMode = false;
    bool heaterAutoMode = false;
    bool phSetpointValid = false;
    float phSetpoint = 0.0f;
    bool orpSetpointValid = false;
    float orpSetpointMv = 0.0f;
    bool heaterSetpointValid = false;
    float heaterSetpointC = 0.0f;
    bool filtrationAutoModeSinceValid = false;
    uint32_t filtrationAutoModeSinceMs = 0;
    bool winterModeSinceValid = false;
    uint32_t winterModeSinceMs = 0;
    bool phAutoModeSinceValid = false;
    uint32_t phAutoModeSinceMs = 0;
    bool orpAutoModeSinceValid = false;
    uint32_t orpAutoModeSinceMs = 0;
};

/** Effective PoolLogic device assignments, read after configuration is loaded. */
struct PoolDeviceAssignments {
    uint8_t filtration = UINT8_MAX;
    uint8_t phPump = UINT8_MAX;
    uint8_t disinfectionPump = UINT8_MAX;
    uint8_t robot = UINT8_MAX;
    uint8_t filling = UINT8_MAX;
    uint8_t chlorineGenerator = UINT8_MAX;
    uint8_t heater = UINT8_MAX;
};

struct PoolConfigurationService {
    bool (*getCharacteristics)(void* ctx, PoolCharacteristics* outCharacteristics);
    bool (*getOperatingConfiguration)(void* ctx,
                                      PoolOperatingConfiguration* outConfiguration);
    void* ctx;
    // Boot-time consumer: HA discovery freezes this mapping until the next boot.
    bool (*getDeviceAssignments)(void* ctx, PoolDeviceAssignments* outAssignments) = nullptr;
    // A false read means this slot has no configuration-owned setpoint.
    bool (*getDeviceSetpoint)(void* ctx, uint8_t slot, float* out) = nullptr;
    bool (*setDeviceSetpoint)(void* ctx, uint8_t slot, float value) = nullptr;
};

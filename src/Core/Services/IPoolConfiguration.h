#pragma once
/**
 * @file IPoolConfiguration.h
 * @brief Read-only pool characteristics shared with history and insight modules.
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
};

struct PoolConfigurationService {
    bool (*getCharacteristics)(void* ctx, PoolCharacteristics* outCharacteristics);
    bool (*getOperatingConfiguration)(void* ctx,
                                      PoolOperatingConfiguration* outConfiguration);
    void* ctx;
};

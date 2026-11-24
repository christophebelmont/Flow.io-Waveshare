#pragma once
#include <stdint.h>

// Stable protocol values shared by ConfigStore, MQTT and local interfaces.
enum class PoolOperatingMode : uint8_t { Manual = 0, Automatic = 1, Winter = 2 };
enum class PoolPhCorrection : uint8_t { Disabled = 0, Minus = 1, Plus = 2 };

constexpr bool poolModeValid(uint8_t value) { return value <= uint8_t(PoolOperatingMode::Winter); }
constexpr bool poolPhCorrectionValid(uint8_t value) { return value <= uint8_t(PoolPhCorrection::Plus); }
constexpr bool poolFiltrationAutomatic(PoolOperatingMode mode) { return mode == PoolOperatingMode::Automatic || mode == PoolOperatingMode::Winter; }
constexpr bool poolRegulationAutomatic(PoolOperatingMode mode) { return mode == PoolOperatingMode::Automatic; }
constexpr bool poolFrostProtection(PoolOperatingMode mode) { return mode == PoolOperatingMode::Winter; }
constexpr bool poolPhAutomatic(PoolOperatingMode mode, PoolPhCorrection correction) {
    return poolRegulationAutomatic(mode) && correction != PoolPhCorrection::Disabled;
}
constexpr const char* poolOperatingModeName(PoolOperatingMode mode) {
    switch (mode) {
        case PoolOperatingMode::Manual: return "Manu";
        case PoolOperatingMode::Automatic: return "Auto";
        case PoolOperatingMode::Winter: return "Hivers";
    }
    return "Manu";
}

struct LegacyPoolModes {
    bool enabled = false;
    bool automatic = false;
    bool winter = false;
    bool phAutomatic = false;
    bool phPlus = false;
    bool disinfectionAutomatic = false;
};
constexpr PoolOperatingMode migratePoolOperatingMode(const LegacyPoolModes& legacy) {
    return !legacy.enabled || !legacy.automatic ? PoolOperatingMode::Manual :
        legacy.winter ? PoolOperatingMode::Winter : PoolOperatingMode::Automatic;
}
constexpr PoolPhCorrection migratePoolPhCorrection(const LegacyPoolModes& legacy) {
    return !legacy.phAutomatic ? PoolPhCorrection::Disabled :
        legacy.phPlus ? PoolPhCorrection::Plus : PoolPhCorrection::Minus;
}

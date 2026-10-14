#pragma once

#include <stddef.h>
#include <stdint.h>

enum class I2cProbeResult : uint8_t { Present, Absent, Error };
inline I2cProbeResult i2cProbeResult(bool present) { return present ? I2cProbeResult::Present : I2cProbeResult::Absent; }
inline I2cProbeResult i2cProbeResult(I2cProbeResult result) { return result; }

enum class I2cAddressStatus : uint8_t {
    Disabled, Invalid, PrimaryConflict, Missing, SecondaryReserved,
    SecondaryConflict, Primary, Secondary, DetectionFailed
};

struct I2cAddressSelection {
    uint8_t primary = 0;
    uint8_t secondary = 0; // Zero disables fallback.
    bool enabled = false;
    uint8_t active = 0; // Runtime only; never replaces persisted configuration.
    I2cAddressStatus status = I2cAddressStatus::Disabled;
};

inline bool validI2cDeviceAddress(uint8_t address)
{
    return address >= 0x08 && address <= 0x77;
}

/** Resolve the complete bus plan before any device is initialized.
 * Enabled primaries remain reserved even without an ACK. Ambiguous addresses
 * are rejected for all contenders, independently of enumeration order.
 * The caller supplies a locked bus probe (or a presence snapshot).
 */
template <typename Probe>
void resolveI2cAddresses(I2cAddressSelection* devices, size_t count, Probe probe)
{
    for (size_t i = 0; i < count; ++i) {
        auto& device = devices[i];
        device.active = 0;
        device.status = I2cAddressStatus::Disabled;
        if (!device.enabled) continue;
        device.status = I2cAddressStatus::Invalid;
        if (!validI2cDeviceAddress(device.primary) ||
            (device.secondary && !validI2cDeviceAddress(device.secondary))) continue;
        device.status = I2cAddressStatus::Missing;
        for (size_t j = 0; j < count; ++j) {
            if (i != j && devices[j].enabled && devices[j].primary == device.primary) {
                device.status = I2cAddressStatus::PrimaryConflict;
                break;
            }
        }
        if (device.status == I2cAddressStatus::Missing) {
            const auto result = i2cProbeResult(probe(device.primary));
            if (result == I2cProbeResult::Present) {
                device.active = device.primary;
                device.status = I2cAddressStatus::Primary;
            } else if (result == I2cProbeResult::Error) {
                device.status = I2cAddressStatus::DetectionFailed;
            }
        }
    }

    // Reserve primaries before considering any secondary.
    for (size_t i = 0; i < count; ++i) {
        auto& device = devices[i];
        if (device.status != I2cAddressStatus::Missing || !device.secondary ||
            device.secondary == device.primary) continue;
        for (size_t j = 0; j < count; ++j) {
            if (i != j && devices[j].enabled && devices[j].primary == device.secondary) {
                device.status = I2cAddressStatus::SecondaryReserved;
                break;
            }
        }
    }

    for (size_t i = 0; i < count; ++i) {
        auto& device = devices[i];
        if (device.status != I2cAddressStatus::Missing || !device.secondary ||
            device.secondary == device.primary) continue;
        bool conflict = false;
        for (size_t j = 0; j < count; ++j) {
            if (i == j || devices[j].secondary != device.secondary) continue;
            if (devices[j].status == I2cAddressStatus::Missing ||
                devices[j].status == I2cAddressStatus::SecondaryConflict) {
                conflict = true;
                break;
            }
        }
        if (conflict) {
            device.status = I2cAddressStatus::SecondaryConflict;
        } else {
            const auto result = i2cProbeResult(probe(device.secondary));
            if (result == I2cProbeResult::Present) {
                device.active = device.secondary;
                device.status = I2cAddressStatus::Secondary;
            } else if (result == I2cProbeResult::Error) {
                device.status = I2cAddressStatus::DetectionFailed;
            }
        }
    }
}

inline const char* i2cAddressStatusName(I2cAddressStatus status)
{
    switch (status) {
        case I2cAddressStatus::DetectionFailed: return "detection failed";
        case I2cAddressStatus::Disabled: return "disabled";
        case I2cAddressStatus::Invalid: return "invalid configuration";
        case I2cAddressStatus::PrimaryConflict: return "primary conflict";
        case I2cAddressStatus::Missing: return "not found";
        case I2cAddressStatus::SecondaryReserved: return "secondary reserved by primary";
        case I2cAddressStatus::SecondaryConflict: return "secondary conflict";
        case I2cAddressStatus::Primary: return "primary";
        case I2cAddressStatus::Secondary: return "secondary";
    }
    return "unknown";
}

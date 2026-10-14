#pragma once
#include <stdint.h>

// Configuration and boot resolution deliberately have separate lifetimes.
struct IOBindingConfig {
    uint16_t primaryPort = 0;
    uint16_t secondaryPort = 0;
};

enum class IOPortPresence : uint8_t {
    Unconfigured, Present, Absent, Disabled, Unsupported, Invalid, DetectionFailed
};
enum class IOBindingSelection : uint8_t { None, Primary, Secondary };
enum class IOBindingReason : uint8_t {
    Unconfigured, PrimarySelected, PrimaryAbsent, PrimaryDisabled,
    PrimaryUnsupported, NoEligiblePort, InvalidConfiguration, DetectionFailed, ResourceConflict
};
struct IOResolvedBinding {
    IOBindingConfig bootConfig{};
    uint16_t port = 0;
    IOBindingSelection selection = IOBindingSelection::None;
    IOBindingReason reason = IOBindingReason::Unconfigured;
    IOPortPresence primaryPresence = IOPortPresence::Unconfigured;
    IOPortPresence secondaryPresence = IOPortPresence::Unconfigured;
};

// Pure boot-time decision: faults are not evidence of a different board variant.
inline IOResolvedBinding resolveIOBinding(const IOBindingConfig& config,
                                         IOPortPresence primary,
                                         IOPortPresence secondary)
{
    IOResolvedBinding result{};
    result.bootConfig = config;
    result.primaryPresence = primary;
    result.secondaryPresence = secondary;
    if (primary == IOPortPresence::Invalid || secondary == IOPortPresence::Invalid ||
        (config.primaryPort && config.primaryPort == config.secondaryPort)) {
        result.reason = IOBindingReason::InvalidConfiguration;
    } else if (primary == IOPortPresence::DetectionFailed) {
        result.reason = IOBindingReason::DetectionFailed;
    } else if (primary == IOPortPresence::Present) {
        result.port = config.primaryPort;
        result.selection = IOBindingSelection::Primary;
        result.reason = IOBindingReason::PrimarySelected;
    } else if (secondary == IOPortPresence::DetectionFailed) {
        result.reason = IOBindingReason::DetectionFailed;
    } else if (secondary == IOPortPresence::Present) {
        result.port = config.secondaryPort;
        result.selection = IOBindingSelection::Secondary;
        result.reason = primary == IOPortPresence::Disabled ? IOBindingReason::PrimaryDisabled :
                        primary == IOPortPresence::Unsupported ? IOBindingReason::PrimaryUnsupported :
                        IOBindingReason::PrimaryAbsent;
    } else if (config.primaryPort || config.secondaryPort) {
        result.reason = IOBindingReason::NoEligiblePort;
    }
    return result;
}
inline const char* ioBindingReasonName(IOBindingReason reason)
{
    switch (reason) {
        case IOBindingReason::Unconfigured: return "unconfigured";
        case IOBindingReason::PrimarySelected: return "primary_selected";
        case IOBindingReason::PrimaryAbsent: return "primary_absent";
        case IOBindingReason::PrimaryDisabled: return "primary_disabled";
        case IOBindingReason::PrimaryUnsupported: return "primary_unsupported";
        case IOBindingReason::NoEligiblePort: return "no_eligible_port";
        case IOBindingReason::InvalidConfiguration: return "invalid_configuration";
        case IOBindingReason::DetectionFailed: return "detection_failed";
        case IOBindingReason::ResourceConflict: return "resource_conflict";
    }
    return "invalid_configuration";
}

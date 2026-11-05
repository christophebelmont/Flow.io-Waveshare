#pragma once
#include "PoolActuatorTypes.h"

// Local output errors retain the original fail-safe policy. An RS485 exchange
// failure degrades communication; only loss beyond the freshness window
// invalidates the operator's demand.
inline bool poolFeedbackInhibitsRun(PoolControlKind kind, const PoolDeviceFeedback& feedback)
{
    if (feedback.error && !feedback.communicationDegraded) return true;
    if (kind == PoolControlKind::Rs485 && feedback.communicationLost) return true;
    return kind != PoolControlKind::Rs485 && feedback.quality == PoolFeedbackQuality::Stale;
}

inline bool poolDependencyPermitsRun(const PoolDriverConfig& config, const PoolDeviceFeedback& parent,
                                     bool parentRequestedOn, bool continuingHeat, uint32_t now)
{
    if (!parentRequestedOn || !parent.observed.running ||
        parent.observed.setpoint < config.dependencyMinimum) return false;
    if (parent.error && !parent.communicationDegraded) return false;
    if (parent.observedValid)
        return !config.requireConfirmedDependency || parent.quality == PoolFeedbackQuality::Confirmed;
    return continuingHeat && config.verifiedFlowGraceMs && parent.hasObservation &&
        now - parent.observedAtMs <= config.verifiedFlowGraceMs;
}

// Without a configured upstream device the local flow switch is the proof
// of circulation. Only an explicitly commissioned hold may use an old sample.
inline bool poolLocalFlowPermitsHeat(const PoolDriverConfig& config, const PoolDeviceFeedback& heater,
                                     bool continuingHeat, uint32_t now)
{
    const auto& t = heater.telemetry;
    if (poolFlowProtectionActive(t) || !(t.raw[0] & 0x0008U)) return false;
    const uint32_t age = now - t.readAtMs[0];
    if ((t.validBlocks & 1U) && age <= config.serial.telemetryStaleMs) return true;
    return continuingHeat && config.verifiedFlowGraceMs && age <= config.verifiedFlowGraceMs;
}

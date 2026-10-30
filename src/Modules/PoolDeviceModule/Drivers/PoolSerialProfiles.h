#pragma once
#include "Core/Services/PoolActuatorTypes.h"

/** Manufacturer profiles are resolved in firmware; the UI persists only the model identifier. */
inline bool applyPoolSerialProfile(PoolSerialModel model, PoolDriverConfig& c)
{
    if (model != PoolSerialModel::AquaGem && model != PoolSerialModel::Polytropic) return false;
    c.capabilities = {};
    c.capabilities.kind = PoolControlKind::Rs485;
    c.serial = {};
    auto& s = c.serial;
    s.model = model;
    s.timeoutMs = 500;
    s.hasFeedback = true;
    if (model == PoolSerialModel::AquaGem) {
        c.capabilities.unit = PoolSetpointUnit::Rpm;
        c.capabilities.minimum = 1200; c.capabilities.maximum = 2900; c.capabilities.startup = 2900;
        s.protocol = RegisterWireProtocol::AquaGem;
        s.address = 0xAA; s.line.baud = 1200; s.line.quietMs = 50;
        s.control = PoolSerialControl::SetpointOrStop;
        s.runningSource = PoolRunningSource::DeviceStatus;
        s.setpoint = {3001, 0xD0, RegisterOperation::WriteSingle};
        s.feedback = {2001, 0xC3, RegisterOperation::Read};
        s.stopValue = 1; s.pollMs = 500; s.line.minimumRequestIntervalMs = 500;
        s.line.lateResponseGuardMs = 450;
    } else {
        c.capabilities.unit = PoolSetpointUnit::Celsius;
        c.capabilities.minimum = 0; c.capabilities.maximum = 60; c.capabilities.startup = 25;
        s.address = 17; s.line.baud = 9600;
        s.run = {1000, 6, RegisterOperation::WriteSingle};
        s.setpoint = {1001, 6, RegisterOperation::WriteSingle};
        s.status = {1000, 3, RegisterOperation::Read};
        s.feedback = {1001, 3, RegisterOperation::Read};
        s.runningMask = 0x10; s.rawPerUnit = 10; s.feedbackUnitsPerRaw = .1f;
        // v6: bits 0–3 select mode, bit 4 is ON/OFF (1 = ON); bits 5–15 remain zero.
        s.modes = {7, {{"Heating Smart", 0x15}, {"Heating Boost", 0x16}, {"Heating Eco", 0x17},
                       {"Cooling Smart", 0x12}, {"Cooling Boost", 0x13}, {"Cooling Eco", 0x14}, {"Auto", 0x11}}};
        s.telemetryProfile = PoolTelemetryProfile::HeatPumpPoly;
        s.staleMs = 8000;
    }
    return true;
}

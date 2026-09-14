#pragma once
/**
 * @file IVariableSpeedPump.h
 * @brief Domain service for variable-speed circulation pumps.
 */

#include <stdint.h>

typedef uint8_t VariableSpeedPumpId;
constexpr VariableSpeedPumpId VARIABLE_SPEED_PUMP_INVALID = 0xFFU;

enum VariableSpeedPumpStatus : uint8_t {
    VARIABLE_SPEED_PUMP_OK = 0,
    VARIABLE_SPEED_PUMP_ACCEPTED = 1,
    VARIABLE_SPEED_PUMP_INVALID_ARGUMENT = 2,
    VARIABLE_SPEED_PUMP_UNKNOWN = 3,
    VARIABLE_SPEED_PUMP_DISABLED = 4,
    VARIABLE_SPEED_PUMP_NOT_READY = 5,
    VARIABLE_SPEED_PUMP_COMMUNICATION_ERROR = 6
};

struct VariableSpeedPumpState {
    bool valid = false;
    bool online = false;
    bool running = false;
    bool requestedRunning = false;
    float requestedSpeedPercent = 0.0f;
    float actualSpeedPercent = 0.0f;
    float actualFlowM3h = 0.0f;
    float powerW = 0.0f;
    uint16_t faultCode = 0U;
    uint32_t updatedAtMs = 0U;
};

struct VariableSpeedPumpService {
    uint8_t (*count)(void* ctx);
    VariableSpeedPumpStatus (*readState)(void* ctx,
                                         VariableSpeedPumpId pumpId,
                                         VariableSpeedPumpState* outState);
    VariableSpeedPumpStatus (*setRunning)(void* ctx,
                                          VariableSpeedPumpId pumpId,
                                          bool running);
    VariableSpeedPumpStatus (*setSpeedPercent)(void* ctx,
                                               VariableSpeedPumpId pumpId,
                                               float speedPercent);
    void* ctx;
};

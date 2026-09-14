#pragma once
/**
 * @file IVariableSpeedPumpDriver.h
 * @brief Hardware-independent driver contract for a variable-speed pump.
 */

#include <stdint.h>

#include "Core/Services/IModbusMaster.h"
#include "Core/Services/IVariableSpeedPump.h"

class IVariableSpeedPumpDriver {
public:
    virtual ~IVariableSpeedPumpDriver() = default;
    virtual bool begin(const ModbusMasterService* modbus) = 0;
    virtual void tick(uint32_t nowMs) = 0;
    virtual bool readState(VariableSpeedPumpState& outState) const = 0;
    virtual VariableSpeedPumpStatus setRunning(bool running) = 0;
    virtual VariableSpeedPumpStatus setSpeedPercent(float speedPercent) = 0;
};

#pragma once
/**
 * @file IFlowCfg.h
 * @brief Flow status domain identifiers shared by the local web routes.
 */

#include <stddef.h>
#include <stdint.h>

#include "Core/RuntimeUi.h"

enum class FlowStatusDomain : uint8_t {
    System = 1,
    Wifi = 2,
    Mqtt = 3,
    I2c = 4,
    Pool = 5,
    Alarm = 6
};

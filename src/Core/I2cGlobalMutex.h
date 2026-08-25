#pragma once
/**
 * @file I2cGlobalMutex.h
 * @brief Global I2C bus mutex shared across modules using `Wire`.
 */

#include <Arduino.h>

bool flowI2cGlobalLock(uint32_t timeoutMs);
void flowI2cGlobalUnlock();


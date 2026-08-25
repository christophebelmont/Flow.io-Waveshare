/**
 * @file I2cGlobalMutex.cpp
 * @brief Global I2C bus mutex shared across modules using `Wire`.
 */

#include "Core/I2cGlobalMutex.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

SemaphoreHandle_t gI2cGlobalMutex = nullptr;

SemaphoreHandle_t ensureMutex_()
{
    if (!gI2cGlobalMutex) {
        gI2cGlobalMutex = xSemaphoreCreateMutex();
    }
    return gI2cGlobalMutex;
}

}  // namespace

bool flowI2cGlobalLock(uint32_t timeoutMs)
{
    SemaphoreHandle_t mutex = ensureMutex_();
    if (!mutex) return false;
    return xSemaphoreTake(mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void flowI2cGlobalUnlock()
{
    if (!gI2cGlobalMutex) return;
    xSemaphoreGive(gI2cGlobalMutex);
}


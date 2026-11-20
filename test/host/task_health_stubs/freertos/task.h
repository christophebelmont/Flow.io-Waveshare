#pragma once

#include "freertos/FreeRTOS.h"

using TaskHandle_t = void*;
inline constexpr unsigned configTICK_RATE_HZ = 1000;
inline TaskHandle_t fakeCurrentTask = nullptr;
inline unsigned fakeStackFree = 0;
inline unsigned (*fakeStackRead)(TaskHandle_t) = nullptr;
inline TickType_t fakeTaskTick = 0;
inline TickType_t xTaskGetTickCount() { return fakeTaskTick; }

inline TaskHandle_t xTaskGetCurrentTaskHandle() { return fakeCurrentTask; }
inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t handle) {
    return fakeStackRead ? fakeStackRead(handle) : fakeStackFree;
}

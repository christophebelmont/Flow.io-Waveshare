#pragma once
#include "FreeRTOS.h"
struct StaticSemaphore_t {};
using SemaphoreHandle_t = StaticSemaphore_t*;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s) { return s; }
inline int xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline void xSemaphoreGive(SemaphoreHandle_t) {}

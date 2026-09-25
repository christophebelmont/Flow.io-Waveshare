#pragma once
#include "FreeRTOS.h"
using StaticSemaphore_t = std::mutex;
using SemaphoreHandle_t = StaticSemaphore_t*;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* m) { return m; }
inline int xSemaphoreTake(SemaphoreHandle_t m, TickType_t) { m->lock(); return pdTRUE; }
inline void xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); }

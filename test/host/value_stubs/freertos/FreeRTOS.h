#pragma once
#include <stdint.h>
#include <mutex>
using TickType_t = uint32_t;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(x) (x)
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()

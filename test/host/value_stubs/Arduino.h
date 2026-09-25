#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
constexpr int INPUT_PULLUP=1, INPUT_PULLDOWN=2, INPUT=0, HIGH=1, LOW=0;
inline uint32_t fakeMs = 0;
inline int fakeLevel = 0;
inline void pinMode(uint8_t, int) {}
inline int digitalRead(uint8_t) { return fakeLevel; }
inline uint32_t millis() { return fakeMs; }

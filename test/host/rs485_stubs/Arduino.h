#pragma once
#include "../stubs/Arduino.h"
inline uint32_t micros() { return testNowMs * 1000U; }
constexpr int OUTPUT = 1, HIGH = 1, LOW = 0;
constexpr uint32_t SERIAL_8N1 = 0, SERIAL_8N2 = 1, SERIAL_8E1 = 2,
                   SERIAL_8E2 = 3, SERIAL_8O1 = 4, SERIAL_8O2 = 5;
inline void pinMode(uint8_t, int) {}
inline void digitalWrite(uint8_t, int) {}

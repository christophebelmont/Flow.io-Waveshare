#pragma once
#include <stdint.h>
extern uint32_t testNowMs;
inline uint32_t millis() { return testNowMs; }

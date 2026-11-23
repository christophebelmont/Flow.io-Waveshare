#pragma once
#include <stdint.h>
#include <math.h>

// Rate-only, volatile measurement. Unlike PulseRuntime this object never stores
// a lifetime pulse count and never participates in PulseCheckpoint / NVS.
struct PulseRateOnly {
    static constexpr uint32_t WindowMs = 1000U;
    uint64_t previousRaw = 0;
    uint64_t windowPulses = 0; // Only pulses seen since the current rate sample.
    uint32_t windowStartMs = 0;
    float pulsesPerMinute = 0.0f;
    bool initialized = false;
    bool valid = false;

    bool sample(uint64_t raw, uint32_t nowMs) {
        if (!initialized) {
            initialized = true;
            previousRaw = raw;
            windowStartMs = nowMs;
            pulsesPerMinute = 0.0f;
            valid = true;
            return true;
        }
        if (raw < previousRaw) {  // PCNT restart/reinitialization.
            previousRaw = raw;
            windowPulses = 0;
            windowStartMs = nowMs;
            pulsesPerMinute = 0.0f;
            valid = true;
            return true;
        }
        const uint64_t delta = raw - previousRaw;
        previousRaw = raw;
        if (UINT64_MAX - windowPulses < delta) {
            windowPulses = 0;
            pulsesPerMinute = 0.0f;
            valid = false;
            return false;
        }
        windowPulses += delta;
        const uint32_t elapsed = nowMs - windowStartMs;
        if (elapsed >= WindowMs) {
            const double rate = (double)windowPulses * 60000.0 / (double)elapsed;
            pulsesPerMinute = (isfinite(rate) && rate <= 3.402823466e38)
                ? (float)rate : 0.0f;
            valid = isfinite(rate) && rate <= 3.402823466e38;
            windowPulses = 0;
            windowStartMs = nowMs;
        }
        return valid;
    }
};

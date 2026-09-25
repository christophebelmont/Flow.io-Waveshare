#pragma once
#include <stdint.h>
#include <limits.h>

/** Task-owned cumulative state. Driver remains monotonic across checkpoints. */
struct PulseRuntime {
    static constexpr uint32_t RateWindowMs = 1000;
    static constexpr uint32_t RateTimeoutMs = 60000;
    uint64_t count = 0, previousRaw = 0, windowCount = 0;
    uint32_t generation = 0, windowMs = 0, lastPulseMs = 0, observedPeriodMs = 0;
    float rate = 0;
    bool initialized = false, rateValid = false, overflow = false, pulseSeen = false;
    void restore(uint64_t base, uint32_t epoch, uint32_t now) {
        *this = PulseRuntime{}; count = base; generation = epoch;
        windowMs = lastPulseMs = now; initialized = true;
    }
    void reset(uint64_t raw, uint32_t epoch, uint32_t now) {
        restore(0, epoch, now); previousRaw = raw;
    }
    bool sample(uint64_t raw, uint32_t now) {
        if (!initialized) restore(0, 0, now);
        if (raw < previousRaw) {
            ++generation; previousRaw = raw; windowCount = 0;
            windowMs = now; rateValid = false; return false;
        }
        const uint64_t delta = raw - previousRaw;
        previousRaw = raw;
        if (UINT64_MAX - count < delta || UINT64_MAX - windowCount < delta) {
            overflow = true; rateValid = false; return false;
        }
        count += delta; windowCount += delta;
        if (delta) {
            if (pulseSeen && delta == 1) observedPeriodMs = now - lastPulseMs;
            lastPulseMs = now; pulseSeen = true;
        }
        const uint32_t elapsed = now - windowMs;
        if (elapsed >= RateWindowMs) {
            if (windowCount >= 2 || observedPeriodMs == 0) {
                rate = static_cast<float>(static_cast<double>(windowCount) * 60000.0 / elapsed);
                rateValid = windowCount > 0 || !pulseSeen;
            } else {
                const uint32_t age = now - lastPulseMs;
                const uint32_t interval = age > observedPeriodMs ? age : observedPeriodMs;
                rate = 60000.0f / interval; rateValid = true;
            }
            windowCount = 0; windowMs = now;
        }
        if (static_cast<uint32_t>(now - lastPulseMs) >= RateTimeoutMs) {
            rate = 0; rateValid = true;
        }
        return !overflow;
    }
};

#pragma once

#include "Core/SaturatingCounter.h"

/** Elapsed time samples from a monotonic clock. Owned by the measuring task. */
struct DurationSamples {
    uint64_t lastUs = 0;
    uint64_t maxUs = 0;
    uint32_t count = 0;

    void record(uint64_t elapsedUs)
    {
        lastUs = elapsedUs;
        if (elapsedUs > maxUs) maxUs = elapsedUs;
        incrementSaturated(count);
    }
};

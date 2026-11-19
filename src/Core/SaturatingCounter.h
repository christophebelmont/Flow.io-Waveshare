#pragma once

#include <stdint.h>

inline void incrementSaturated(uint32_t& counter)
{
    if (counter < UINT32_MAX) ++counter;
}

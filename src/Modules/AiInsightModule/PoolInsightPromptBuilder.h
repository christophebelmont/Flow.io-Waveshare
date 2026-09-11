#pragma once
/**
 * @file PoolInsightPromptBuilder.h
 * @brief Bounded, allocation-free preparation of the pool insight prompt.
 */

#include "Core/Services/IAiInsight.h"
#include "Core/Services/IPoolHistory.h"

#include <stddef.h>

class PoolInsightPromptBuilder {
public:
    static const char* instructions();
    static bool build(const PoolHistorySnapshot* history,
                      const AiWeatherStatus& weather,
                      char* weatherText,
                      size_t weatherTextCapacity,
                      char* prompt,
                      size_t promptCapacity);
};

#pragma once
/**
 * @file PoolHistoryPersistence.h
 * @brief Versioned compact persistence format for one daily history record.
 */

#include <stddef.h>
#include <stdint.h>

#include "Modules/PoolHistoryModule/PoolHistoryAccumulator.h"

namespace PoolHistoryPersistence {

/** Fixed version-3 record. Older NVS formats are intentionally unsupported. */
constexpr size_t EncodedSize = 368U;

bool encode(const PoolHistoryDayState& state,
            uint8_t* out,
            size_t outCapacity,
            size_t& outLength);

bool decode(const uint8_t* encoded,
            size_t encodedLength,
            PoolHistoryDayState& outState);

}  // namespace PoolHistoryPersistence

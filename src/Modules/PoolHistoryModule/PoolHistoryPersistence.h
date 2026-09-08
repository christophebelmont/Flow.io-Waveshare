#pragma once
/**
 * @file PoolHistoryPersistence.h
 * @brief Versioned compact persistence format for one daily history record.
 */

#include <stddef.h>
#include <stdint.h>

#include "Modules/PoolHistoryModule/PoolHistoryAccumulator.h"

namespace PoolHistoryPersistence {

constexpr size_t LegacyEncodedSize = 168U;
/** Fixed encoded size for version 2, including day/night temperature and refill. */
constexpr size_t EncodedSize = 236U;

bool encode(const PoolHistoryDayState& state,
            uint8_t* out,
            size_t outCapacity,
            size_t& outLength);

bool decode(const uint8_t* encoded,
            size_t encodedLength,
            PoolHistoryDayState& outState);

}  // namespace PoolHistoryPersistence

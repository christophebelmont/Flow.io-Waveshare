#pragma once
#include "ValueRegistry.h"

/** Stable, read-only internal source catalog. IDs are independent of display names. */
namespace InternalAnalogSources {
static_assert(ValueIds::Derived + ValueIds::DerivedCapacity <= ValueIds::PoolTank, "Value ranges overlap");
enum class TankMetric : uint8_t { RemainingMl, FillPercent };
constexpr uint64_t FreshnessMs = 5000;
constexpr ValueId tankId(uint8_t slot, TankMetric metric) {
    return slot < ValueIds::PoolTankSlots
        ? ValueIds::PoolTank + 2 * slot + static_cast<uint8_t>(metric) : VALUE_INVALID;
}
constexpr bool contains(ValueId id) {
    return id >= ValueIds::PoolTank && id < ValueIds::Capacity;
}
constexpr const char* unit(ValueId id) {
    return contains(id) ? ((id - ValueIds::PoolTank) % 2 == 0 ? "mL" : "%") : "";
}
inline bool define(ValueRegistry& registry) {
    for (ValueId id = ValueIds::PoolTank; id < ValueIds::Capacity; ++id) {
        ValueMetadata meta{};
        setValueUnit(meta.displayUnit, unit(id));
        meta.precision = 1;
        if (!registry.define(id, meta)) return false;
    }
    return true;
}
inline bool fresh(const ValueSnapshot& snapshot, uint64_t nowMs) {
    return snapshot.quality == ValueQuality::Valid && nowMs >= snapshot.timestampMs &&
           nowMs - snapshot.timestampMs <= FreshnessMs && isfinite(snapshot.value.f);
}
/** Called by the equipment owner. Invalid configuration must never become an empty tank. */
inline void publishTank(ValueRegistry& registry, uint8_t slot, float remainingMl,
                        float capacityMl, bool available, uint64_t nowMs) {
    const bool valid = available && isfinite(capacityMl) && capacityMl > 0 &&
        isfinite(remainingMl) && remainingMl >= 0 && remainingMl <= capacityMl;
    const auto quality = valid ? ValueQuality::Valid : ValueQuality::Invalid;
    ValueNumber remaining{}, fill{};
    if (valid) {
        remaining.f = remainingMl;
        fill.f = 100.0f * (remainingMl / capacityMl);
    }
    registry.write(tankId(slot, TankMetric::RemainingMl), remaining, nowMs, quality);
    registry.write(tankId(slot, TankMetric::FillPercent), fill, nowMs, quality);
}
}

#pragma once
#include "ValueRegistry.h"

/** Stable, read-only internal source catalog. IDs are independent of display names. */
namespace InternalAnalogSources {
static_assert(ValueIds::Derived + ValueIds::DerivedCapacity <= ValueIds::PoolTank, "Value ranges overlap");
enum class TankMetric : uint8_t { RemainingL, FillPercent };
constexpr uint64_t FreshnessMs = 5000;
constexpr ValueId tankId(uint8_t slot, TankMetric metric) {
    return slot < ValueIds::PoolTankSlots
        ? ValueIds::PoolTank + 2 * slot + static_cast<uint8_t>(metric) : VALUE_INVALID;
}
constexpr bool contains(ValueId id) {
    return id >= ValueIds::PoolTank && id < ValueIds::PoolUptime;
}
constexpr const char* unit(ValueId id) {
    return contains(id) ? ((id - ValueIds::PoolTank) % 2 == 0 ? "L" : "%") : "";
}
inline bool define(ValueRegistry& registry) {
    for (ValueId id = ValueIds::PoolTank; id < ValueIds::PoolUptime; ++id) {
        ValueMetadata meta{};
        setValueUnit(meta.displayUnit, unit(id));
        meta.precision = ((id - ValueIds::PoolTank) % 2 == 0) ? 3 : 1;
        meta.maxAgeMs = FreshnessMs;
        if (!registry.define(id, meta)) return false;
    }
    return true;
}
inline bool fresh(const ValueSnapshot& snapshot, uint64_t nowMs) {
    return snapshot.quality == ValueQuality::Valid && nowMs >= snapshot.timestampMs &&
           nowMs - snapshot.timestampMs <= FreshnessMs && isfinite(snapshot.value.f);
}
/** Called by the equipment owner. Invalid configuration must never become an empty tank. */
inline void publishTank(ValueRegistry& registry, uint8_t slot, float remainingL,
                        float capacityL, bool available, uint64_t nowMs) {
    const bool valid = available && isfinite(capacityL) && capacityL > 0 &&
        isfinite(remainingL) && remainingL >= 0 && remainingL <= capacityL;
    const auto quality = valid ? ValueQuality::Valid : ValueQuality::Invalid;
    ValueNumber remaining{}, fill{};
    if (valid) {
        remaining.f = remainingL;
        fill.f = 100.0f * (remainingL / capacityL);
    }
    registry.write(tankId(slot, TankMetric::RemainingL), remaining, nowMs, quality);
    registry.write(tankId(slot, TankMetric::FillPercent), fill, nowMs, quality);
}
}

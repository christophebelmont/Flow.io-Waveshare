#pragma once
#include "ValueRegistry.h"
#include "InternalAnalogSources.h"
#include <stdio.h>

namespace PoolDeviceValues {
constexpr ValueId uptimeId(uint8_t slot) {
    return slot < ValueIds::PoolTankSlots ? ValueIds::PoolUptime + slot : VALUE_INVALID;
}
constexpr bool isUptime(ValueId id) {
    return id >= ValueIds::PoolUptime && id < ValueIds::DigitalState;
}
inline bool define(ValueRegistry& values) {
    for (uint8_t slot = 0; slot < ValueIds::PoolTankSlots; ++slot) {
        ValueMetadata metadata{};
        metadata.type = ValueType::UInt64;
        metadata.aggregation = AggregationMode::Counter;
        setValueUnit(metadata.displayUnit, "s");
        metadata.precision = 0;
        metadata.maxAgeMs = 5000;
        if (!values.define(uptimeId(slot), metadata)) return false;
    }
    return true;
}
/** Shared catalogue labels, prepared before HA discovery and used by all displays. */
inline void setNames(ValueRegistry& values, uint8_t slot, const char* equipmentName) {
    if (slot >= ValueIds::PoolTankSlots || !equipmentName || !equipmentName[0]) return;
    char name[sizeof(ValueMetadata::name)];
    snprintf(name, sizeof(name), "%s · Temps de marche du jour", equipmentName);
    values.setName(uptimeId(slot), name);
    snprintf(name, sizeof(name), "%s · Volume restant", equipmentName);
    values.setName(InternalAnalogSources::tankId(slot, InternalAnalogSources::TankMetric::RemainingL), name);
    snprintf(name, sizeof(name), "%s · Remplissage estimé", equipmentName);
    values.setName(InternalAnalogSources::tankId(slot, InternalAnalogSources::TankMetric::FillPercent), name);
}
inline void publishUptime(ValueRegistry& values, uint8_t slot, uint64_t runningMsDay,
                          bool available, uint64_t timestampMs) {
    ValueNumber value{};
    value.u64 = runningMsDay / 1000ULL;
    values.write(uptimeId(slot), value, timestampMs,
                 available ? ValueQuality::Valid : ValueQuality::Invalid);
}
}

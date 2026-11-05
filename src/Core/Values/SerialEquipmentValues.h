#pragma once
#include "ValueRegistry.h"
#include "Core/Generated/DataValueCatalog_Generated.h"
#include "Core/Services/PoolActuatorTypes.h"
#include <stdio.h>

/** Boot-time declarations and read-only publication of the driver's existing samples. */
namespace SerialEquipmentValues {
constexpr ValueId id(uint8_t slot, EquipmentMetric metric) {
    if (slot >= EquipmentValueSlots) return VALUE_INVALID;
    const uint8_t metricId = uint8_t(metric);
    if (metricId < EquipmentValueLegacyStride)
        return ValueIds::Equipment + slot * EquipmentValueLegacyStride + metricId;
    return ValueIds::Equipment + EquipmentValueSlots * EquipmentValueLegacyStride +
        slot * (EquipmentValueStride - EquipmentValueLegacyStride) + metricId - EquipmentValueLegacyStride;
}
template<typename Store>
inline bool define(Store& store, uint8_t slot, const char* name, const PoolDriverConfig& config) {
    if (config.capabilities.kind != PoolControlKind::Rs485) return true;
    auto declare = [&](EquipmentMetric metric, const char* unit, bool binary, uint32_t maxAge, int8_t precision) {
        const auto identity = id(slot, metric);
        const auto* descriptor = dataValueDescriptor(identity);
        if (!descriptor) return false;
        ValueMetadata metadata{};
        metadata.type = binary ? ValueType::Bool : ValueType::Float;
        metadata.precision = precision;
        metadata.maxAgeMs = maxAge;
        setValueUnit(metadata.displayUnit, unit);
        snprintf(metadata.name, sizeof(metadata.name), "%s · %s", name, descriptor->label);
        return store.defineDataValue(identity, metadata);
    };
    const char* unit = poolSetpointUnitSymbol(config.capabilities.unit);
    if (!declare(EquipmentMetric::Requested, unit, false, 5000,
                 config.capabilities.unit == PoolSetpointUnit::Rpm ? 0 : 1)) return false;
    if (config.serial.hasFeedback &&
        (!declare(EquipmentMetric::Observed, unit, false, config.serial.staleMs,
                  config.capabilities.unit == PoolSetpointUnit::Rpm ? 0 : 1) ||
         !declare(EquipmentMetric::Running, "", true, config.serial.staleMs, 0))) return false;
    if (config.serial.telemetryProfile == PoolTelemetryProfile::HeatPumpPoly)
        for (const auto& field : kHeatPumpPolyFields)
            if (!declare(field.metric, field.unit, field.mask != 0, config.serial.telemetryStaleMs,
                         field.mask || field.scale >= 1.0f ? 0 : 1)) return false;
    return true;
}
inline void publish(ValueRegistry& values, uint8_t slot, const PoolDriverConfig& config,
                    const PoolDeviceTarget& target, const PoolDeviceFeedback& feedback,
                    bool enabled, uint32_t nowMs, uint64_t monotonicMs) {
    if (config.capabilities.kind != PoolControlKind::Rs485) return;
    auto write = [&](EquipmentMetric metric, float number, bool valid, uint32_t readAt, bool periodic = false) {
        const auto identity = id(slot, metric);
        ValueSnapshot previous{}; ValueMetadata metadata{};
        if (!values.read(identity, previous, &metadata)) return;
        const uint64_t age = uint32_t(nowMs - readAt);
        const auto quality = valid && age <= monotonicMs ? ValueQuality::Valid : ValueQuality::Invalid;
        const uint64_t timestamp = quality == ValueQuality::Valid ? monotonicMs - age : monotonicMs;
        ValueNumber value{};
        if (metadata.type == ValueType::Bool) value.b = number != 0; else value.f = number;
        const bool changed = quality != previous.quality || !valueEqual(metadata.type, value, previous.value);
        if (!changed && previous.sequence && (quality != ValueQuality::Valid ||
            (periodic ? timestamp - previous.timestampMs < 1000 : timestamp <= previous.timestampMs))) return;
        values.write(identity, value, timestamp, quality);
    };
    const bool serial = enabled && config.capabilities.kind == PoolControlKind::Rs485;
    write(EquipmentMetric::Requested, target.setpoint, serial, nowMs, true);
    const bool observed = serial && config.serial.hasFeedback && feedback.observedValid &&
        feedback.quality == PoolFeedbackQuality::Confirmed &&
        uint32_t(nowMs - feedback.observedAtMs) <= config.serial.staleMs;
    write(EquipmentMetric::Observed, feedback.observed.setpoint, observed, feedback.observedAtMs);
    write(EquipmentMetric::Running, feedback.observed.running, observed, feedback.observedAtMs);
    for (const auto& field : kHeatPumpPolyFields) {
        const auto readAt = feedback.telemetry.readAtMs[field.block];
        const bool valid = serial && config.serial.telemetryProfile == PoolTelemetryProfile::HeatPumpPoly &&
            (feedback.telemetry.validBlocks & (1U << field.block)) &&
            uint32_t(nowMs - readAt) <= config.serial.telemetryStaleMs;
        write(field.metric, poolTelemetryValue(feedback.telemetry, field), valid, readAt);
    }
}
}

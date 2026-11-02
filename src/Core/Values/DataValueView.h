#pragma once
#include "Core/Generated/RuntimeUiManifest_Generated.h"
#include "Core/Generated/DataValueCatalog_Generated.h"
#include "Core/Values/DataValueAvailability.h"
#include <stdio.h>
#include <esp_timer.h>

enum class DataValueFreshness : uint8_t { Unknown, Valid, Degraded, Stale };

inline DataValueFreshness classifyDataValueFreshness(bool hasSample,
                                                      ValueQuality quality,
                                                      uint64_t ageMs,
                                                      uint32_t maxAgeMs)
{
    if (!hasSample || quality == ValueQuality::Unknown) return DataValueFreshness::Unknown;
    if (maxAgeMs > 0U && ageMs > maxAgeMs) return DataValueFreshness::Stale;
    return quality == ValueQuality::Valid ? DataValueFreshness::Valid : DataValueFreshness::Degraded;
}

/** Descriptor lookup and reads over the existing registry, with no value cache. */
inline ValueId runtimeDataValueId(RuntimeUiId id) {
    const auto* item = findRuntimeUiManifestItem(id);
    return item ? item->dataValueId : VALUE_INVALID;
}
inline bool readRuntimeDataValue(const ValueRegistry& values, RuntimeUiId id,
                                 ValueSnapshot& value, ValueMetadata& metadata) {
    const auto source = runtimeDataValueId(id);
    return readAvailableDataValue(values, source, value, metadata) && metadata.usable;
}
inline const char* dataValueLabel(ValueId id, const ValueMetadata& metadata) {
    if (metadata.name[0]) return metadata.name;
    const auto* descriptor = dataValueDescriptor(id);
    return descriptor ? descriptor->label : "";
}

struct DataValuePresentation {
    bool available = false;
    bool hasSample = false;
    DataValueFreshness freshness = DataValueFreshness::Unknown;
    uint64_t sampledAtMs = 0U;
    uint32_t ageMs = 0U;
    RuntimeUiWireType wireType = RuntimeUiWireType::Unavailable;
    bool boolValue = false;
    int32_t i32Value = 0;
    uint32_t u32Value = 0;
    float f32Value = 0;
    double f64Value = 0;
    char stringValue[64]{};
};
inline bool presentDataValueById(const ValueRegistry& values, ValueId id,
                                 DataValuePresentation& out, ValueMetadata& metadata,
                                 bool requireUsable = true) {
    ValueSnapshot value{};
    if (!values.read(id, value, &metadata) || (requireUsable && !metadata.usable)) return false;
    out.hasSample = value.sequence != 0U;
    out.sampledAtMs = value.timestampMs;
    const uint64_t nowMs = (uint64_t)esp_timer_get_time() / 1000ULL;
    const uint64_t age = nowMs >= value.timestampMs ? nowMs - value.timestampMs : 0U;
    out.ageMs = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
    out.freshness = classifyDataValueFreshness(out.hasSample, value.quality, age, metadata.maxAgeMs);
    out.available = out.freshness == DataValueFreshness::Valid;
    if (!out.hasSample) return false;
    if (metadata.type == ValueType::UInt64) {
        out.wireType = RuntimeUiWireType::String;
        snprintf(out.stringValue, sizeof(out.stringValue), "%llu", (unsigned long long)value.value.u64);
    } else if (metadata.type == ValueType::Bool) {
        out.wireType = RuntimeUiWireType::Bool;
        out.boolValue = value.value.b;
    } else {
        out.wireType = RuntimeUiWireType::Float64;
        out.f64Value = valueAsDouble(metadata.type, value.value);
    }
    return true;
}

inline bool presentDataValue(const ValueRegistry& values, RuntimeUiId id,
                             DataValuePresentation& out, ValueMetadata& metadata) {
    const ValueId source = runtimeDataValueId(id);
    if (source == VALUE_INVALID || !presentDataValueById(values, source, out, metadata)) return false;
    const auto* descriptor = dataValueDescriptor(source);
    if (!descriptor || !dataValueCompatible(*descriptor, metadata)) {
        out.available = false;
        return false;
    }
    return out.available;
}

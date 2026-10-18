#pragma once
#include "Core/Generated/RuntimeUiManifest_Generated.h"
#include "Core/Generated/DataValueCatalog_Generated.h"
#include "Core/Values/DataValueAvailability.h"
#include <stdio.h>

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
    RuntimeUiWireType wireType = RuntimeUiWireType::Unavailable;
    bool boolValue = false;
    int32_t i32Value = 0;
    uint32_t u32Value = 0;
    float f32Value = 0;
    double f64Value = 0;
    char stringValue[64]{};
};
inline bool presentDataValue(const ValueRegistry& values, RuntimeUiId id,
                             DataValuePresentation& out, ValueMetadata& metadata) {
    ValueSnapshot value{};
    if (!readRuntimeDataValue(values, id, value, metadata)) return false;
    out.available = true;
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

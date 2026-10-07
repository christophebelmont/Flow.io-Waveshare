#pragma once
#include "Value.h"
#include <stdio.h>

/** Shared bounded numeric presentation. Preserve doubles and never emit NaN/Inf
 * or a truncated number. Large values use round-trip scientific notation. */
inline bool formatValueNumber(char* out, size_t capacity, double value, int precision) {
    if (!out || !capacity) return false;
    out[0] = '\0';
    if (!isfinite(value)) return false;
    value = roundToPrecision(value, precision);
    if (precision > VALUE_PRECISION_MAX) precision = VALUE_PRECISION_MAX;
    // Fixed notation is only attempted when its integral part fits comfortably.
    int length = precision >= 0 && fabs(value) < 1e16
        ? snprintf(out, capacity, "%.*f", precision, value)
        : snprintf(out, capacity, "%.17g", value);
    if (length >= 0 && size_t(length) < capacity) return true;
    length = snprintf(out, capacity, "%.17g", value);
    if (length >= 0 && size_t(length) < capacity) return true;
    out[0] = '\0';
    return false;
}

inline bool formatValueSnapshot(char* out, size_t capacity, ValueId id,
                                const ValueSnapshot& sample, const ValueMetadata& metadata) {
    char number[40] = "null";
    if (sample.quality == ValueQuality::Valid &&
        !formatValueNumber(number, sizeof(number), valueAsDouble(metadata.type, sample.value), metadata.precision))
        return false;
    const int length = snprintf(out, capacity,
        "{\"id\":%u,\"value\":%s,\"quality\":%u,\"generation\":%lu}",
        unsigned(id), number, unsigned(sample.quality), (unsigned long)sample.generation);
    return length >= 0 && size_t(length) < capacity;
}

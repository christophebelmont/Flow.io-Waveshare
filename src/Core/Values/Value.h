#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

using ValueId = uint16_t;
constexpr ValueId VALUE_INVALID = UINT16_MAX;
namespace ValueIds {
constexpr ValueId Analog = 0, Digital = 32, PulseRate = 48, Total = 64,
                  ConvertedRate = 80, Derived = 96;
constexpr uint8_t DerivedCapacity = 5;
constexpr uint16_t Capacity = Derived + DerivedCapacity;
}
enum class ValueType : uint8_t { Bool, Int32, UInt32, UInt64, Float, Double };
enum class AggregationMode : uint8_t { Gauge, Rate, Counter };
enum class ValueQuality : uint8_t { Unknown, Valid, Invalid };
enum class ValueUnit : uint8_t { Unspecified, Pulse, PulsePerMinute };
union ValueNumber {
    bool b;
    int32_t i32;
    uint32_t u32;
    uint64_t u64;
    float f;
    double d;
    constexpr ValueNumber() : u64(0) {}
};
// Display unit text (e.g. "°C", "mV", "L"). Empty means no unit.
constexpr uint8_t UnitTextCapacity = 8;

inline void setValueUnit(char (&dest)[UnitTextCapacity], const char* unit) {
    size_t i = 0;
    if (unit) {
        for (; i + 1U < UnitTextCapacity && unit[i] != '\0'; ++i) dest[i] = unit[i];
    }
    dest[i] = '\0';
}

// Display/notification quantization: decimals kept for a value. Negative means
// no quantization (raw double). The stored value keeps full precision; only the
// change announcement and the published/displayed value use it.
constexpr int8_t VALUE_PRECISION_NONE = -1;
constexpr int8_t VALUE_PRECISION_MAX = 6;

inline double roundToPrecision(double value, int precision) {
    if (precision < 0) return value;
    if (precision > VALUE_PRECISION_MAX) precision = VALUE_PRECISION_MAX;
    double scale = 1.0;
    for (int i = 0; i < precision; ++i) scale *= 10.0;
    // At this magnitude a double has no fractional digits to round.
    if (!isfinite(value) || fabs(value) >= 0x1p52 / scale) return value;
    return round(value * scale) / scale;
}

struct ValueMetadata {
    ValueType type = ValueType::Float;
    AggregationMode aggregation = AggregationMode::Gauge;
    ValueUnit unit = ValueUnit::Unspecified;
    ValueId source = VALUE_INVALID;
    double scale = 1.0;
    double offset = 0.0;
    char displayUnit[UnitTextCapacity] = {0};
    int8_t precision = VALUE_PRECISION_NONE;
};
struct ValueSnapshot {
    ValueNumber value{};
    uint64_t timestampMs = 0;
    uint32_t sequence = 0;
    uint32_t generation = 0;
    ValueQuality quality = ValueQuality::Unknown;
    // Finite difference since the previous observation, before rounding the
    // published absolute value. Invalid across initialization/recalibration/reset.
    double delta = 0;
    bool deltaValid = false;
};
inline double valueAsDouble(ValueType type, const ValueNumber& value) {
    switch (type) {
        case ValueType::Bool: return value.b ? 1.0 : 0.0;
        case ValueType::Int32: return value.i32;
        case ValueType::UInt32: return value.u32;
        case ValueType::UInt64: return static_cast<double>(value.u64);
        case ValueType::Float: return value.f;
        case ValueType::Double: return value.d;
    }
    return NAN;
}
inline bool valueEqual(ValueType type, const ValueNumber& a, const ValueNumber& b) {
    switch (type) {
        case ValueType::Bool: return a.b == b.b;
        case ValueType::Int32: return a.i32 == b.i32;
        case ValueType::UInt32: return a.u32 == b.u32;
        case ValueType::UInt64: return a.u64 == b.u64;
        case ValueType::Float: return a.f == b.f;
        case ValueType::Double: return a.d == b.d;
    }
    return false;
}

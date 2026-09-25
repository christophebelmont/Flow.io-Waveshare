#pragma once
#include <stdint.h>
#include <math.h>

using ValueId = uint16_t;
constexpr ValueId VALUE_INVALID = UINT16_MAX;
namespace ValueIds {
constexpr ValueId Analog = 0, Digital = 32, PulseRate = 48, Total = 64,
                  ConvertedRate = 80, Derived = 96;
constexpr uint16_t Capacity = 112;
constexpr uint8_t DerivedCapacity = 16;
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
struct ValueMetadata {
    ValueType type = ValueType::Float;
    AggregationMode aggregation = AggregationMode::Gauge;
    ValueUnit unit = ValueUnit::Unspecified;
    ValueId source = VALUE_INVALID;
    double scale = 1.0;
    double offset = 0.0;
};
struct ValueSnapshot {
    ValueNumber value{};
    uint64_t timestampMs = 0;
    uint32_t sequence = 0;
    uint32_t generation = 0;
    ValueQuality quality = ValueQuality::Unknown;
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

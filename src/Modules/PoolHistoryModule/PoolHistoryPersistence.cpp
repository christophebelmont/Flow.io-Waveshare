/**
 * @file PoolHistoryPersistence.cpp
 * @brief Versioned compact persistence format for daily pool history.
 */

#include "Modules/PoolHistoryModule/PoolHistoryPersistence.h"

#include <math.h>
#include <string.h>

namespace PoolHistoryPersistence {
namespace {

constexpr uint32_t kMagic = 0x31534850UL;  // "PHS1" remains the format-family magic.
constexpr uint16_t kVersion = 3U;
constexpr uint16_t kFlagValid = 0x0001U;
constexpr uint16_t kFlagComplete = 0x0002U;
constexpr uint16_t kFlagRefillVolumeValid = 0x0004U;
constexpr uint16_t kFlagRefillStateObserved = 0x0008U;
constexpr uint64_t kMaximumDayDurationMs = 48ULL * 60ULL * 60ULL * 1000ULL;

class Writer {
public:
    Writer(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}

    bool putU16(uint16_t value) {
        return putByte_((uint8_t)value) && putByte_((uint8_t)(value >> 8U));
    }
    bool putU32(uint32_t value) {
        for (uint8_t shift = 0U; shift < 32U; shift += 8U) {
            if (!putByte_((uint8_t)(value >> shift))) return false;
        }
        return true;
    }
    bool putU64(uint64_t value) {
        for (uint8_t shift = 0U; shift < 64U; shift += 8U) {
            if (!putByte_((uint8_t)(value >> shift))) return false;
        }
        return true;
    }
    bool putFloat(float value) {
        static_assert(sizeof(float) == sizeof(uint32_t), "32-bit float required");
        uint32_t bits = 0U;
        memcpy(&bits, &value, sizeof(bits));
        return putU32(bits);
    }
    bool putDouble(double value) {
        static_assert(sizeof(double) == sizeof(uint64_t), "64-bit double required");
        uint64_t bits = 0U;
        memcpy(&bits, &value, sizeof(bits));
        return putU64(bits);
    }
    size_t size() const { return offset_; }

private:
    bool putByte_(uint8_t value) {
        if (!data_ || offset_ >= capacity_) return false;
        data_[offset_++] = value;
        return true;
    }

    uint8_t* data_ = nullptr;
    size_t capacity_ = 0U;
    size_t offset_ = 0U;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t length) : data_(data), length_(length) {}

    bool getU16(uint16_t& value) {
        uint8_t b0 = 0U;
        uint8_t b1 = 0U;
        if (!getByte_(b0) || !getByte_(b1)) return false;
        value = (uint16_t)b0 | ((uint16_t)b1 << 8U);
        return true;
    }
    bool getU32(uint32_t& value) {
        value = 0U;
        for (uint8_t shift = 0U; shift < 32U; shift += 8U) {
            uint8_t byte = 0U;
            if (!getByte_(byte)) return false;
            value |= ((uint32_t)byte << shift);
        }
        return true;
    }
    bool getU64(uint64_t& value) {
        value = 0U;
        for (uint8_t shift = 0U; shift < 64U; shift += 8U) {
            uint8_t byte = 0U;
            if (!getByte_(byte)) return false;
            value |= ((uint64_t)byte << shift);
        }
        return true;
    }
    bool getFloat(float& value) {
        uint32_t bits = 0U;
        if (!getU32(bits)) return false;
        memcpy(&value, &bits, sizeof(value));
        return true;
    }
    bool getDouble(double& value) {
        uint64_t bits = 0U;
        if (!getU64(bits)) return false;
        memcpy(&value, &bits, sizeof(value));
        return true;
    }
    size_t size() const { return offset_; }

private:
    bool getByte_(uint8_t& value) {
        if (!data_ || offset_ >= length_) return false;
        value = data_[offset_++];
        return true;
    }

    const uint8_t* data_ = nullptr;
    size_t length_ = 0U;
    size_t offset_ = 0U;
};

uint32_t checksum_(const uint8_t* data, size_t length)
{
    uint32_t hash = 2166136261UL;
    for (size_t i = 0U; i < length; ++i) {
        hash ^= data[i];
        hash *= 16777619UL;
    }
    return hash;
}

bool plausibleDate_(uint32_t localDate)
{
    const uint32_t year = localDate / 10000UL;
    const uint32_t month = (localDate / 100UL) % 100UL;
    const uint32_t day = localDate % 100UL;
    return year >= 2021U && year <= 2199U && month >= 1U && month <= 12U &&
           day >= 1U && day <= 31U;
}

bool validMetric_(const PoolHistoryMetricState& metric)
{
    if (!isfinite(metric.sum)) return false;
    if (metric.sampleCount == 0U) return true;
    return isfinite(metric.first) && isfinite(metric.last) &&
           isfinite(metric.minimum) && isfinite(metric.maximum) &&
           metric.minimum <= metric.maximum &&
           metric.first >= metric.minimum && metric.first <= metric.maximum &&
           metric.last >= metric.minimum && metric.last <= metric.maximum;
}

bool validActivity_(const PoolHistoryActivityState& activity)
{
    for (uint8_t i = 0U; i < POOL_HISTORY_DAY_PERIOD_COUNT; ++i) {
        if (activity.runningMs[i] > activity.observedMs[i]) return false;
    }
    return true;
}

bool validState_(const PoolHistoryDayState& state)
{
    if (!state.valid || !plausibleDate_(state.localDate) || state.dayStartUtc == 0U) return false;
    if ((state.observedFromUtc == 0U) != (state.observedUntilUtc == 0U)) return false;
    if (state.observedFromUtc > state.observedUntilUtc) return false;
    if (!validActivity_(state.filtration) || !validActivity_(state.heating)) return false;
    uint64_t filtrationObservedMs = 0U;
    uint64_t heatingObservedMs = 0U;
    for (uint8_t i = 0U; i < POOL_HISTORY_DAY_PERIOD_COUNT; ++i) {
        filtrationObservedMs += state.filtration.observedMs[i];
        heatingObservedMs += state.heating.observedMs[i];
    }
    if (filtrationObservedMs > kMaximumDayDurationMs ||
        heatingObservedMs > kMaximumDayDurationMs) return false;
    for (uint8_t i = 0U; i < (uint8_t)PoolHistoryMetric::Count; ++i) {
        if (!validMetric_(state.metrics[i])) return false;
    }
    if (!validMetric_(state.daytimeWaterTemperature) ||
        !validMetric_(state.nighttimeWaterTemperature) ||
        !isfinite(state.refillVolumeLitres) || state.refillVolumeLitres < 0.0) {
        return false;
    }
    return true;
}

bool writeMetric_(Writer& writer, const PoolHistoryMetricState& metric)
{
    return writer.putU32(metric.sampleCount) && writer.putFloat(metric.first) &&
           writer.putFloat(metric.last) && writer.putFloat(metric.minimum) &&
           writer.putFloat(metric.maximum) && writer.putDouble(metric.sum);
}

bool readMetric_(Reader& reader, PoolHistoryMetricState& metric)
{
    return reader.getU32(metric.sampleCount) && reader.getFloat(metric.first) &&
           reader.getFloat(metric.last) && reader.getFloat(metric.minimum) &&
           reader.getFloat(metric.maximum) && reader.getDouble(metric.sum);
}

bool writeActivity_(Writer& writer, const PoolHistoryActivityState& activity)
{
    for (uint8_t i = 0U; i < POOL_HISTORY_DAY_PERIOD_COUNT; ++i) {
        if (!writer.putU32(activity.runningMs[i]) ||
            !writer.putU32(activity.observedMs[i])) return false;
    }
    return true;
}

bool readActivity_(Reader& reader, PoolHistoryActivityState& activity)
{
    for (uint8_t i = 0U; i < POOL_HISTORY_DAY_PERIOD_COUNT; ++i) {
        if (!reader.getU32(activity.runningMs[i]) ||
            !reader.getU32(activity.observedMs[i])) return false;
    }
    return true;
}

}  // namespace

bool encode(const PoolHistoryDayState& state,
            uint8_t* out,
            size_t outCapacity,
            size_t& outLength)
{
    outLength = 0U;
    if (!out || outCapacity < EncodedSize || !validState_(state)) return false;

    Writer writer(out, outCapacity);
    uint16_t flags = kFlagValid;
    if (state.complete) flags |= kFlagComplete;
    if (state.refillVolumeValid) flags |= kFlagRefillVolumeValid;
    if (state.refillStateObserved) flags |= kFlagRefillStateObserved;

    if (!writer.putU32(kMagic) || !writer.putU16(kVersion) || !writer.putU16(flags) ||
        !writer.putU32(state.localDate) || !writer.putU64(state.dayStartUtc) ||
        !writer.putU64(state.observedFromUtc) || !writer.putU64(state.observedUntilUtc) ||
        !writeActivity_(writer, state.filtration) ||
        !writeActivity_(writer, state.heating)) {
        return false;
    }

    for (uint8_t i = 0U; i < (uint8_t)PoolHistoryMetric::Count; ++i) {
        if (!writeMetric_(writer, state.metrics[i])) return false;
    }
    if (!writeMetric_(writer, state.daytimeWaterTemperature) ||
        !writeMetric_(writer, state.nighttimeWaterTemperature) ||
        !writer.putDouble(state.refillVolumeLitres) ||
        !writer.putU32(state.refillEventCount)) return false;

    if (writer.size() + sizeof(uint32_t) != EncodedSize) return false;
    if (!writer.putU32(checksum_(out, writer.size()))) return false;
    outLength = writer.size();
    return outLength == EncodedSize;
}

bool decode(const uint8_t* encoded,
            size_t encodedLength,
            PoolHistoryDayState& outState)
{
    outState = PoolHistoryDayState{};
    if (!encoded || encodedLength != EncodedSize) return false;

    const size_t checksumOffset = encodedLength - sizeof(uint32_t);
    Reader checksumReader(encoded + checksumOffset, sizeof(uint32_t));
    uint32_t storedChecksum = 0U;
    if (!checksumReader.getU32(storedChecksum) ||
        storedChecksum != checksum_(encoded, checksumOffset)) {
        return false;
    }

    Reader reader(encoded, checksumOffset);
    uint32_t magic = 0U;
    uint16_t version = 0U;
    uint16_t flags = 0U;
    if (!reader.getU32(magic) || !reader.getU16(version) || !reader.getU16(flags) ||
        magic != kMagic || version != kVersion ||
        (flags & kFlagValid) == 0U) {
        return false;
    }

    PoolHistoryDayState decoded{};
    decoded.valid = true;
    decoded.complete = (flags & kFlagComplete) != 0U;
    if (!reader.getU32(decoded.localDate) || !reader.getU64(decoded.dayStartUtc) ||
        !reader.getU64(decoded.observedFromUtc) || !reader.getU64(decoded.observedUntilUtc) ||
        !readActivity_(reader, decoded.filtration) ||
        !readActivity_(reader, decoded.heating)) {
        return false;
    }

    for (uint8_t i = 0U; i < (uint8_t)PoolHistoryMetric::Count; ++i) {
        if (!readMetric_(reader, decoded.metrics[i])) return false;
    }
    if (!readMetric_(reader, decoded.daytimeWaterTemperature) ||
        !readMetric_(reader, decoded.nighttimeWaterTemperature) ||
        !reader.getDouble(decoded.refillVolumeLitres) ||
        !reader.getU32(decoded.refillEventCount)) return false;
    decoded.refillVolumeValid = (flags & kFlagRefillVolumeValid) != 0U;
    decoded.refillStateObserved = (flags & kFlagRefillStateObserved) != 0U;

    if (reader.size() != checksumOffset || !validState_(decoded)) return false;
    outState = decoded;
    return true;
}

}  // namespace PoolHistoryPersistence

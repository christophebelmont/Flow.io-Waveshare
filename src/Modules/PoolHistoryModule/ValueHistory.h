#pragma once
#include "Core/Values/Value.h"
#include <stddef.h>

/** UTC hour/day records. Coverage and boundary uncertainty are explicit. */
struct ValueHistoryRecord {
    uint64_t period = 0, durationMs = 0;
    double minimum = 0, maximum = 0, weightedSum = 0;
    double start = 0, end = 0, delta = 0;
    uint64_t rawStart = 0, rawEnd = 0, rawDelta = 0;
    uint32_t discontinuities = 0;
    bool valid = false, boundaryUncertain = false;
    double average() const { return durationMs ? weightedSum / durationMs : NAN; }
    void gauge(double value, uint64_t duration);
    void merge(const ValueHistoryRecord& other);
};

/** Memory supplied once by PoolHistoryModule; no allocation in observation path.
 * The caller serializes observe/tick/read. Values are addressed directly by ID. */
class ValueHistory {
public:
    static constexpr uint8_t PsramHours = 24, PsramDays = 7;
    static constexpr uint8_t InternalHours = 2, InternalDays = 1;
    static constexpr uint64_t HourMs = 3600000, MaximumAgeMs = 900000;
    static size_t bytes(uint8_t hours, uint8_t days);
    void initialize(void* memory, uint8_t hours, uint8_t days);
    void clock(uint64_t monotonicMs, uint64_t epochMs);
    void observe(ValueId id, const ValueMetadata& metadata, const ValueSnapshot& sample,
                 const ValueSnapshot* source);
    void tick(uint64_t monotonicMs);
    bool read(ValueId id, bool daily, uint8_t age, ValueHistoryRecord& out) const;
    uint16_t used() const { return used_; }
private:
    struct Slot {
        ValueHistoryRecord hour{}, day{};
        ValueSnapshot previous{};
        ValueMetadata metadata{};
        uint64_t previousRaw = 0, integratedUntil = 0, observedEpoch = 0;
        bool rawCounter = false, observed = false;
    };
    void advance_(ValueId id, uint64_t epochMs);
    void rotate_(ValueId id, uint64_t hour);
    Slot* slots_ = nullptr;
    ValueHistoryRecord* hours_ = nullptr;
    ValueHistoryRecord* days_ = nullptr;
    uint64_t anchorMono_ = 0, anchorEpoch_ = 0;
    uint8_t hourCount_ = 0, dayCount_ = 0;
    uint16_t used_ = 0;
};

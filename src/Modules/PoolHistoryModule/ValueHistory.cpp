#include "ValueHistory.h"
#include <new>
#include <algorithm>

void ValueHistoryRecord::gauge(double value, uint64_t duration) {
    if (!duration) return;
    if (!valid) { minimum = maximum = start = value; valid = true; }
    minimum = std::min(minimum, value); maximum = std::max(maximum, value);
    end = value; weightedSum += value * duration; durationMs += duration;
}
void ValueHistoryRecord::merge(const ValueHistoryRecord& other) {
    discontinuities += other.discontinuities;
    boundaryUncertain |= other.boundaryUncertain;
    if (!other.valid) return;
    if (!valid) {
        minimum = other.minimum; maximum = other.maximum;
        start = other.start; rawStart = other.rawStart; valid = true;
    } else {
        minimum = std::min(minimum, other.minimum); maximum = std::max(maximum, other.maximum);
    }
    end = other.end; rawEnd = other.rawEnd;
    delta += other.delta; rawDelta += other.rawDelta;
    weightedSum += other.weightedSum; durationMs += other.durationMs;
}
size_t ValueHistory::bytes(uint8_t hours, uint8_t days) {
    return sizeof(Slot) * ValueIds::Capacity + sizeof(ValueHistoryRecord) * ValueIds::Capacity * (hours + days);
}
void ValueHistory::initialize(void* memory, uint8_t hours, uint8_t days) {
    slots_ = static_cast<Slot*>(memory); hourCount_ = hours; dayCount_ = days;
    hours_ = reinterpret_cast<ValueHistoryRecord*>(slots_ + ValueIds::Capacity);
    days_ = hours_ + ValueIds::Capacity * hours;
    for (uint16_t i = 0; i < ValueIds::Capacity; ++i) new (&slots_[i]) Slot{};
    for (size_t i = 0; i < size_t(ValueIds::Capacity) * (hours + days); ++i) new (&hours_[i]) ValueHistoryRecord{};
}
void ValueHistory::clock(uint64_t mono, uint64_t epoch) {
    if (!slots_) return;
    if (anchorEpoch_) {
        const uint64_t expected = anchorEpoch_ + (mono - anchorMono_);
        const uint64_t drift = expected > epoch ? expected - epoch : epoch - expected;
        if (drift < 2000) return; // Keep fractional monotonic timing across second-resolution clock reads.
        for (uint16_t i = 0; i < ValueIds::Capacity; ++i) {
            auto& s = slots_[i];
            s.previous.quality = ValueQuality::Unknown;
            s.integratedUntil = epoch; ++s.hour.discontinuities;
        }
    }
    anchorMono_ = mono; anchorEpoch_ = epoch;
}
void ValueHistory::rotate_(ValueId id, uint64_t hour) {
    auto& s = slots_[id];
    if (s.hour.period == hour) return;
    if (s.hour.period) {
        hours_[id * hourCount_ + s.hour.period % hourCount_] = s.hour;
        s.day.merge(s.hour);
    }
    const uint64_t day = hour / 24;
    if (s.day.period != day) {
        if (s.day.period) days_[id * dayCount_ + s.day.period % dayCount_] = s.day;
        s.day = {}; s.day.period = day;
    }
    s.hour = {}; s.hour.period = hour;
}
void ValueHistory::advance_(ValueId id, uint64_t epoch) {
    auto& s = slots_[id];
    if (epoch < s.integratedUntil) {
        ++s.hour.discontinuities; s.previous.quality = ValueQuality::Unknown;
        s.integratedUntil = epoch;
    }
    uint64_t begin = s.integratedUntil;
    const uint64_t until = std::min(epoch, s.observedEpoch + MaximumAgeMs);
    if (s.metadata.aggregation != AggregationMode::Counter && s.previous.quality == ValueQuality::Valid) {
        while (begin < until) {
            rotate_(id, begin / HourMs);
            const uint64_t end = std::min(until, (begin / HourMs + 1) * HourMs);
            s.hour.gauge(valueAsDouble(s.metadata.type, s.previous.value), end - begin);
            begin = end;
        }
    }
    rotate_(id, epoch / HourMs);
    s.integratedUntil = epoch;
}
void ValueHistory::observe(ValueId id, const ValueMetadata& metadata, const ValueSnapshot& sample,
                            const ValueSnapshot* source) {
    if (!slots_ || !anchorEpoch_ || id >= ValueIds::Capacity || sample.timestampMs < anchorMono_) return;
    const uint64_t epoch = anchorEpoch_ + sample.timestampMs - anchorMono_;
    auto& s = slots_[id];
    if (!s.observed) { s.integratedUntil = epoch; s.metadata = metadata; s.observed = true; ++used_; }
    const auto previousHour = s.observedEpoch / HourMs;
    advance_(id, epoch);
    const bool valid = sample.quality == ValueQuality::Valid;
    const bool continuous = valid && s.previous.quality == ValueQuality::Valid &&
        s.previous.generation == sample.generation;
    // Direct affine counters preserve the integer subtraction before scaling.
    const bool raw = metadata.type == ValueType::UInt64 || source != nullptr;
    // Caller supplies UInt64 source only for this optimization (see module adapter).
    const uint64_t rawValue = source ? source->value.u64 : sample.value.u64;
    const double current = valueAsDouble(metadata.type, sample.value);
    if (metadata.aggregation == AggregationMode::Counter && valid) {
        auto& record = s.hour;
        if (!record.valid) {
            record.start = continuous ? valueAsDouble(s.metadata.type, s.previous.value) : current;
            record.rawStart = continuous ? s.previousRaw : rawValue;
            record.valid = true;
        }
        record.end = current; record.rawEnd = rawValue;
        if (continuous) {
            const double previous = valueAsDouble(s.metadata.type, s.previous.value);
            if ((raw && rawValue >= s.previousRaw) || (!raw && current >= previous)) {
                if (raw) {
                    const uint64_t delta = rawValue - s.previousRaw;
                    record.rawDelta += delta;
                    record.delta += static_cast<double>(delta) * (source ? metadata.scale : 1.0);
                } else record.delta += current - previous;
                record.durationMs += epoch - s.observedEpoch;
                record.boundaryUncertain |= previousHour != record.period;
            } else ++record.discontinuities;
        } else if (s.previous.sequence) ++record.discontinuities;
    }
    s.previous = sample; s.previousRaw = rawValue;
    s.rawCounter = metadata.type == ValueType::UInt64 || source != nullptr;
    s.metadata = metadata; s.observedEpoch = epoch;
}
void ValueHistory::tick(uint64_t mono) {
    if (!slots_ || !anchorEpoch_ || mono < anchorMono_) return;
    const uint64_t epoch = anchorEpoch_ + mono - anchorMono_;
    for (ValueId id = 0; id < ValueIds::Capacity; ++id) if (slots_[id].observed) advance_(id, epoch);
}
bool ValueHistory::read(ValueId id, bool daily, uint8_t age, ValueHistoryRecord& out) const {
    if (!slots_ || id >= ValueIds::Capacity || !slots_[id].observed) return false;
    const auto& s = slots_[id];
    if (!age) {
        out = daily ? s.day : s.hour;
        if (daily) out.merge(s.hour);
    } else {
        if (age > (daily ? dayCount_ : hourCount_)) return false;
        const auto period = (daily ? s.day.period : s.hour.period) - age;
        out = daily ? days_[id * dayCount_ + period % dayCount_] : hours_[id * hourCount_ + period % hourCount_];
        if (out.period != period) return false;
    }
    return out.valid;
}

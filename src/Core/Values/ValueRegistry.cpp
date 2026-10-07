#include "ValueRegistry.h"
#include <esp_heap_caps.h>
#include <new>
namespace {
struct Lock {
    SemaphoreHandle_t mutex;
    explicit Lock(SemaphoreHandle_t m) : mutex(m) { xSemaphoreTake(mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex); }
};
}
ValueRegistry::~ValueRegistry() {
    if (storage_) {
        storage_->~Storage();
        heap_caps_free(storage_);
    }
}
size_t ValueRegistry::storageBytes() { return sizeof(Storage); }
bool ValueRegistry::begin() {
    Lock lock(mutex_);
    if (storage_) return true;
    void* memory = heap_caps_malloc(sizeof(Storage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    storageInPsram_ = memory != nullptr;
    if (!memory) memory = heap_caps_malloc(sizeof(Storage), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) return false;
    storage_ = new (memory) Storage{};
    return true;
}
bool ValueRegistry::define(ValueId id, const ValueMetadata& metadata) {
    Lock lock(mutex_);
    if (!storage_ || id >= ValueIds::Capacity || storage_->slots[id].used || metadata.source != VALUE_INVALID) return false;
    storage_->slots[id].metadata = metadata;
    storage_->slots[id].used = true;
    ++used_;
    return true;
}
bool ValueRegistry::defineProgram_(ValueId id, const ValueExpression::Program& program,
                                   const double* parameters, const ValueMetadata& metadata) {
    if (!storage_ || id >= ValueIds::Capacity || storage_->slots[id].used ||
        !parameters || transformCount_ == ProgramCapacity || !ValueExpression::validate(program) ||
        metadata.aggregation > AggregationMode::Counter) return false;
    for (uint8_t i = 0; i < ValueExpression::ParamCount; ++i)
        if (!isfinite(parameters[i])) return false;
    // Registration is topological: every dependency must already exist.
    for (uint8_t i = 0; i < program.dependencyCount; ++i)
        if (!storage_->slots[program.dependencies[i]].used ||
            !ValueExpression::validSourceType(program.dependencies[i],
                storage_->slots[program.dependencies[i]].metadata.type)) return false;
    auto& slot = storage_->slots[id];
    slot.metadata = metadata; slot.used = true; slot.program = transformCount_;
    auto& transform = storage_->programs[transformCount_];
    transform.program = program;
    for (uint8_t i = 0; i < ValueExpression::ParamCount; ++i) transform.parameters[i] = parameters[i];
    storage_->transforms[transformCount_++] = id;
    ++used_;
    uint64_t timestamp = 0;
    for (uint8_t i = 0; i < program.dependencyCount; ++i)
        if (storage_->slots[program.dependencies[i]].runtime.timestampMs > timestamp)
            timestamp = storage_->slots[program.dependencies[i]].runtime.timestampMs;
    propagate_(VALUE_INVALID, timestamp, id);
    return true;
}
bool ValueRegistry::defineProgram(ValueId id, const ValueExpression::Program& program,
                                  const double* parameters, AggregationMode mode,
                                  const char* displayUnit, int8_t precision) {
    Lock lock(mutex_);
    ValueMetadata metadata{ValueType::Double, mode};
    setValueUnit(metadata.displayUnit, displayUnit);
    metadata.precision = precision;
    return defineProgram_(id, program, parameters, metadata);
}
bool ValueRegistry::defineAffine(ValueId id, ValueId source, double scale, double offset,
                                 AggregationMode mode) {
    Lock lock(mutex_);
    if (mode == AggregationMode::Counter && scale < 0) return false;
    const double parameters[ValueExpression::ParamCount] = {scale, offset, 0, 0};
    return defineProgram_(id, ValueExpression::affine(source), parameters,
        {ValueType::Double, mode, ValueUnit::Unspecified, source, scale, offset});
}
bool ValueRegistry::setParameters_(ValueId id, const double* parameters, uint64_t timestampMs) {
    if (!storage_ || id >= ValueIds::Capacity || !storage_->slots[id].used ||
        storage_->slots[id].program == UINT8_MAX || !parameters) return false;
    auto& slot = storage_->slots[id];
    auto& transform = storage_->programs[slot.program];
    bool valid = true, same = true;
    for (uint8_t i = 0; i < ValueExpression::ParamCount; ++i) {
        valid &= isfinite(parameters[i]);
        same &= parameters[i] == transform.parameters[i] ||
            (isnan(parameters[i]) && isnan(transform.parameters[i]));
    }
    if (slot.metadata.source != VALUE_INVALID && slot.metadata.aggregation == AggregationMode::Counter)
        valid &= parameters[0] >= 0;
    if (same && valid == transform.valid) {
        // Constant-only programs have no producer to refresh history coverage.
        constexpr uint64_t ConstantObservationMs = 1000;
        if (!transform.program.dependencyCount && timestampMs >= slot.runtime.timestampMs &&
            timestampMs - slot.runtime.timestampMs >= ConstantObservationMs)
            propagate_(VALUE_INVALID, timestampMs, id);
        return valid;
    }
    transform.valid = valid;
    for (uint8_t i = 0; i < ValueExpression::ParamCount; ++i) transform.parameters[i] = parameters[i];
    if (slot.metadata.source != VALUE_INVALID) {
        slot.metadata.scale = parameters[0]; slot.metadata.offset = parameters[1];
    }
    ++slot.runtime.generation;
    slot.runtime.quality = ValueQuality::Unknown;
    propagate_(VALUE_INVALID, timestampMs, id);
    return valid;
}
bool ValueRegistry::setParameters(ValueId id, const double* parameters, uint64_t timestampMs) {
    Lock lock(mutex_);
    return setParameters_(id, parameters, timestampMs);
}
bool ValueRegistry::setAffine(ValueId id, double scale, double offset) {
    Lock lock(mutex_);
    if (!storage_ || id >= ValueIds::Capacity || storage_->slots[id].metadata.source == VALUE_INVALID) return false;
    const double parameters[ValueExpression::ParamCount] = {scale, offset, 0, 0};
    return setParameters_(id, parameters, storage_->slots[id].runtime.timestampMs);
}
bool ValueRegistry::read(ValueId id, ValueSnapshot& out, ValueMetadata* metadata) const {
    Lock lock(mutex_);
    if (!storage_ || id >= ValueIds::Capacity || !storage_->slots[id].used) return false;
    out = storage_->slots[id].runtime;
    if (metadata) *metadata = storage_->slots[id].metadata;
    return true;
}
void ValueRegistry::notify_(ValueId id) {
    if (!observer_) return;
    const auto& slot = storage_->slots[id];
    const ValueSnapshot* source = slot.metadata.source == VALUE_INVALID ||
        storage_->slots[slot.metadata.source].metadata.type != ValueType::UInt64
        ? nullptr : &storage_->slots[slot.metadata.source].runtime;
    observer_(observerContext_, id, slot.metadata, slot.runtime, source);
}
bool ValueRegistry::write(ValueId id, ValueNumber value, uint64_t timestampMs,
                          ValueQuality quality, uint32_t generation) {
    Lock lock(mutex_);
    if (!storage_ || id >= ValueIds::Capacity || !storage_->slots[id].used ||
        storage_->slots[id].program != UINT8_MAX) return false;
    auto& slot = storage_->slots[id];
    if (timestampMs < slot.runtime.timestampMs) return false;
    if (!isfinite(valueAsDouble(slot.metadata.type, value))) quality = ValueQuality::Invalid;
    const bool rootChanged = !valueEqual(slot.metadata.type, value, slot.runtime.value) ||
        quality != slot.runtime.quality || generation != slot.runtime.generation || !slot.runtime.sequence;
    slot.runtime = {value, timestampMs, slot.runtime.sequence + 1, generation, quality};
    notify_(id);
    propagate_(id, timestampMs);
    if (notification_ && rootChanged) notification_(notificationContext_, id, slot.runtime.sequence);
    return true;
}
void ValueRegistry::propagate_(ValueId changedId, uint64_t timestampMs, ValueId force) {
    bool changed[ValueIds::Capacity]{};
    if (changedId != VALUE_INVALID) changed[changedId] = true;
    for (uint16_t i = 0; i < transformCount_; ++i) {
        const auto derivedId = storage_->transforms[i];
        auto& target = storage_->slots[derivedId];
        auto& transform = storage_->programs[target.program];
        const auto& program = transform.program;
        bool evaluate = derivedId == force;
        for (uint8_t d = 0; d < program.dependencyCount; ++d) evaluate |= changed[program.dependencies[d]];
        if (!evaluate) continue;
        const auto old = target.runtime;
        bool continuous = old.quality == ValueQuality::Valid;
        bool discontinuity = false;
        ValueQuality quality = transform.valid ? ValueQuality::Valid : ValueQuality::Invalid;
        ValueExpression::Number inputs[ValueExpression::MaxDependencies];
        for (uint8_t d = 0; d < program.dependencyCount; ++d) {
            const auto& source = storage_->slots[program.dependencies[d]];
            const auto& now = source.runtime;
            const auto& previous = transform.previous[d];
            discontinuity |= now.generation != previous.generation;
            continuous &= previous.quality == ValueQuality::Valid && now.quality == ValueQuality::Valid &&
                now.generation == previous.generation;
            if (now.quality == ValueQuality::Invalid) quality = ValueQuality::Invalid;
            else if (now.quality == ValueQuality::Unknown && quality == ValueQuality::Valid) quality = ValueQuality::Unknown;
            auto& input = inputs[d];
            input.current = valueAsDouble(source.metadata.type, now.value);
            input.previous = valueAsDouble(source.metadata.type, previous.value);
            if (now.sequence == previous.sequence) input.delta = 0;
            else if (source.metadata.type == ValueType::UInt64) {
                // Subtract integers before conversion, including a possible decreasing gauge.
                input.delta = now.value.u64 >= previous.value.u64
                    ? static_cast<double>(now.value.u64 - previous.value.u64)
                    : -static_cast<double>(previous.value.u64 - now.value.u64);
                if (source.metadata.aggregation == AggregationMode::Counter && now.value.u64 < previous.value.u64)
                    discontinuity = true;
            } else if (source.program != UINT8_MAX && now.deltaValid) input.delta = now.delta;
            else input.delta = input.current - input.previous;
            transform.previous[d] = now;
        }
        ValueExpression::Number result;
        const bool validResult = ValueExpression::evaluate(program, transform.parameters, inputs, result);
        if (!validResult && quality == ValueQuality::Valid) quality = ValueQuality::Invalid;
        if (continuous && validResult && target.metadata.aggregation == AggregationMode::Counter) {
            if (!isfinite(result.delta)) quality = ValueQuality::Invalid;
            else if (result.delta < 0) discontinuity = true;
        }
        if (discontinuity) ++target.runtime.generation;
        target.runtime.value.d = validResult ? result.current : NAN;
        target.runtime.timestampMs = timestampMs > old.timestampMs ? timestampMs : old.timestampMs;
        target.runtime.quality = quality;
        target.runtime.delta = result.delta;
        target.runtime.deltaValid = continuous && !discontinuity && quality == ValueQuality::Valid && isfinite(result.delta);
        ++target.runtime.sequence;
        changed[derivedId] = true;
        notify_(derivedId);
        // Change announcement honours the value precision: a new double that rounds to the
        // same displayed value is not announced. The stored value keeps full precision.
        const double oldQuantized = roundToPrecision(valueAsDouble(target.metadata.type, old.value),
                                                     target.metadata.precision);
        const double newQuantized = roundToPrecision(valueAsDouble(target.metadata.type, target.runtime.value),
                                                     target.metadata.precision);
        if (notification_ && (oldQuantized != newQuantized ||
            old.quality != target.runtime.quality || old.generation != target.runtime.generation ||
            derivedId == force || !old.sequence))
            notification_(notificationContext_, derivedId, target.runtime.sequence);
    }
}
void ValueRegistry::setObserver(Observer callback, void* context) {
    Lock lock(mutex_); observer_ = callback; observerContext_ = context;
}
uint16_t ValueRegistry::used() const { Lock lock(mutex_); return used_; }

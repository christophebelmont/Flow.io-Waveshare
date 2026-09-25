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
bool ValueRegistry::defineAffine(ValueId id, ValueId source, double scale, double offset,
                                 AggregationMode mode) {
    Lock lock(mutex_);
    // Sources must already exist: registration order is the topological order.
    if (!storage_ || id >= ValueIds::Capacity || source >= ValueIds::Capacity || storage_->slots[id].used ||
        !storage_->slots[source].used || !isfinite(scale) || !isfinite(offset) ||
        (mode == AggregationMode::Counter && scale < 0.0)) return false;
    storage_->slots[id].metadata = {ValueType::Double, mode, ValueUnit::Unspecified, source, scale, offset};
    storage_->slots[id].used = true;
    storage_->transforms[transformCount_++] = id;
    ++used_;
    return true;
}
bool ValueRegistry::setAffine(ValueId id, double scale, double offset) {
    Lock lock(mutex_);
    if (!storage_ || id >= ValueIds::Capacity || !storage_->slots[id].used || storage_->slots[id].metadata.source == VALUE_INVALID) return false;
    auto& slot = storage_->slots[id];
    const bool valid = isfinite(scale) && isfinite(offset) &&
        (slot.metadata.aggregation != AggregationMode::Counter || scale >= 0);
    if (!valid) {
        if (slot.transformValid) ++slot.runtime.generation;
        slot.transformValid = false;
        return false;
    }
    if (slot.transformValid && slot.metadata.scale == scale && slot.metadata.offset == offset) return true;
    slot.transformValid = true;
    slot.metadata.scale = scale; slot.metadata.offset = offset;
    ++slot.runtime.generation;
    slot.runtime.quality = ValueQuality::Unknown;
    return true;
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
        storage_->slots[id].metadata.source != VALUE_INVALID) return false;
    auto& slot = storage_->slots[id];
    if (timestampMs < slot.runtime.timestampMs) return false;
    if (!isfinite(valueAsDouble(slot.metadata.type, value))) quality = ValueQuality::Invalid;
    const bool rootChanged = !valueEqual(slot.metadata.type, value, slot.runtime.value) ||
        quality != slot.runtime.quality || generation != slot.runtime.generation || !slot.runtime.sequence;
    slot.runtime = {value, timestampMs, slot.runtime.sequence + 1, generation, quality};
    notify_(id);
    bool changed[ValueIds::Capacity]{};
    changed[id] = true;
    for (uint16_t i = 0; i < transformCount_; ++i) {
        auto derivedId = storage_->transforms[i];
        auto& target = storage_->slots[derivedId];
        auto& source = storage_->slots[target.metadata.source];
        if (!changed[target.metadata.source]) continue;
        ValueNumber result;
        result.d = valueAsDouble(source.metadata.type, source.runtime.value) * target.metadata.scale + target.metadata.offset;
        const auto old = target.runtime;
        // A source reset and a calibration change are separate discontinuities.
        if (source.runtime.generation != storage_->sourceGenerations[derivedId]) {
            ++target.runtime.generation;
            storage_->sourceGenerations[derivedId] = source.runtime.generation;
        }
        target.runtime.value = result;
        target.runtime.timestampMs = timestampMs;
        target.runtime.quality = target.transformValid && isfinite(result.d) ? source.runtime.quality : ValueQuality::Invalid;
        ++target.runtime.sequence;
        changed[derivedId] = true;
        notify_(derivedId);
        if (notification_ && (!valueEqual(target.metadata.type, old.value, target.runtime.value) ||
            old.quality != target.runtime.quality || old.generation != target.runtime.generation || !old.sequence))
            notification_(notificationContext_, derivedId, target.runtime.sequence);
    }
    if (notification_ && rootChanged) notification_(notificationContext_, id, slot.runtime.sequence);
    return true;
}
void ValueRegistry::setObserver(Observer callback, void* context) {
    Lock lock(mutex_); observer_ = callback; observerContext_ = context;
}
uint16_t ValueRegistry::used() const { Lock lock(mutex_); return used_; }

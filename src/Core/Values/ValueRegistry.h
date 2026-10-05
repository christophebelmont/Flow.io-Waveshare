#pragma once
#include "Value.h"
#include "ValueExpression.h"
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/** Direct index storage. Definitions are assembled before producers start.
 * Observer runs synchronously under the task mutex, never in ISR context, and
 * must not call back into this registry. This preserves every observation even
 * when the EventBus notification queue is full. One fixed allocation at boot; no runtime allocation. */
class ValueRegistry {
public:
    using Observer = void (*)(void*, ValueId, const ValueMetadata&, const ValueSnapshot&,
                             const ValueSnapshot* source);
    ValueRegistry() : mutex_(xSemaphoreCreateMutexStatic(&mutexStorage_)) {}
    ~ValueRegistry();
    bool begin();
    bool storageInPsram() const { return storageInPsram_; }
    static size_t storageBytes();
    ValueRegistry(const ValueRegistry&) = delete;
    ValueRegistry& operator=(const ValueRegistry&) = delete;
    bool define(ValueId id, const ValueMetadata& metadata);
    bool defineAffine(ValueId id, ValueId source, double scale, double offset,
                      AggregationMode mode);
    bool setAffine(ValueId id, double scale, double offset);
    bool defineProgram(ValueId id, const ValueExpression::Program& program,
                       const double* parameters, AggregationMode mode,
                       const char* displayUnit = nullptr,
                       int8_t precision = VALUE_PRECISION_NONE);
    // Also refreshes constant-only observations at most once per second when a
    // monotonic timestamp is supplied, keeping history coverage current.
    bool setParameters(ValueId id, const double* parameters, uint64_t timestampMs = 0);
    bool read(ValueId id, ValueSnapshot& out, ValueMetadata* metadata = nullptr) const;
    bool write(ValueId id, ValueNumber value, uint64_t timestampMs,
               ValueQuality quality = ValueQuality::Valid, uint32_t generation = 0);
    void setObserver(Observer callback, void* context);
    void setNotification(void (*callback)(void*, ValueId, uint32_t), void* context) {
        notification_ = callback; notificationContext_ = context;
    }
    uint16_t used() const;
private:
    struct Slot {
        ValueMetadata metadata{};
        ValueSnapshot runtime{};
        uint8_t program = UINT8_MAX;
        bool used = false;
    };
    // User expressions and 2 conversions for each of the 16 pulse inputs.
    static constexpr uint8_t ProgramCapacity = ValueIds::DerivedCapacity + 32;
    struct Transform {
        ValueExpression::Program program{};
        double parameters[ValueExpression::ParamCount]{};
        ValueSnapshot previous[ValueExpression::MaxDependencies]{};
        bool valid = true;
    };
    bool defineProgram_(ValueId id, const ValueExpression::Program& program,
                        const double* parameters, const ValueMetadata& metadata);
    bool setParameters_(ValueId id, const double* parameters, uint64_t timestampMs);
    void propagate_(ValueId changedId, uint64_t timestampMs, ValueId force = VALUE_INVALID);
    void notify_(ValueId id);
    struct Storage {
        Slot slots[ValueIds::Capacity]{};
        Transform programs[ProgramCapacity]{};
        ValueId transforms[ProgramCapacity]{};
    };
    Storage* storage_ = nullptr;
    bool storageInPsram_ = false;
    void (*notification_)(void*, ValueId, uint32_t) = nullptr;
    void* notificationContext_ = nullptr;
    uint16_t transformCount_ = 0, used_ = 0;
    Observer observer_ = nullptr;
    void* observerContext_ = nullptr;
    mutable StaticSemaphore_t mutexStorage_{};
    SemaphoreHandle_t mutex_;
};

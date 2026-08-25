#pragma once
/**
 * @file RuntimeSnapshotProvider.h
 * @brief Optional interface for modules exposing runtime MQTT snapshots.
 */

#include <stddef.h>
#include <stdint.h>
#include "Core/EventBus/EventPayloads.h"

enum class RuntimeRouteClass : uint8_t {
    NumericThrottled = 0,
    ActuatorImmediate = 1,
};

class IRuntimeSnapshotProvider {
public:
    virtual ~IRuntimeSnapshotProvider() = default;

    /** Number of runtime snapshots exposed by this provider. */
    virtual uint8_t runtimeSnapshotCount() const = 0;
    /** MQTT suffix for snapshot index (example: "rt/io/input/state"). */
    virtual const char* runtimeSnapshotSuffix(uint8_t idx) const = 0;
    /** Route policy class used by the runtime dispatcher. */
    virtual RuntimeRouteClass runtimeSnapshotClass(uint8_t idx) const = 0;
    /** Whether this snapshot is affected by the provided DataStore key change. */
    virtual bool runtimeSnapshotAffectsKey(uint8_t idx, DataKey key) const = 0;
    /** Build snapshot payload and return associated change timestamp. */
    virtual bool buildRuntimeSnapshot(uint8_t idx, char* out, size_t len, uint32_t& maxTsOut) const = 0;
};

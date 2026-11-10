#pragma once
/**
 * @file PoolDeviceRuntime.h
 * @brief Pool device runtime helpers and keys.
 */

#include <stdint.h>
#include <string.h>
#include "Core/DataStore/DataStore.h"
#include "Core/EventBus/EventPayloads.h"
#include "Core/DataKeys.h"
#include "Modules/PoolDeviceModule/PoolDeviceModuleDataModel.h"

// RUNTIME_PUBLIC

constexpr DataKey DATAKEY_POOL_DEVICE_STATE_BASE = DataKeys::PoolDeviceStateBase;
constexpr DataKey DATAKEY_POOL_DEVICE_METRICS_BASE = DataKeys::PoolDeviceMetricsBase;
static_assert(POOL_DEVICE_MAX <= DataKeys::PoolDeviceStateReservedCount, "DataKeys::PoolDeviceStateReservedCount too small for pool device slots");
static_assert(POOL_DEVICE_MAX <= DataKeys::PoolDeviceMetricsReservedCount, "DataKeys::PoolDeviceMetricsReservedCount too small for pool device slots");

static inline bool poolDeviceRuntimeState(const DataStore& ds, uint8_t idx, PoolDeviceRuntimeStateEntry& out)
{
    if (idx >= POOL_DEVICE_MAX) return false;
    out = ds.data().pool.state[idx];
    return out.valid;
}

/** Number of registered devices, including disabled devices. */
static inline uint8_t poolDeviceRuntimeCount(const DataStore& ds)
{
    uint8_t count = 0;
    for (uint8_t idx = 0; idx < POOL_DEVICE_MAX; ++idx) {
        if (ds.data().pool.state[idx].valid) ++count;
    }
    return count;
}

static inline bool poolDeviceRuntimeMetrics(const DataStore& ds, uint8_t idx, PoolDeviceRuntimeMetricsEntry& out)
{
    if (idx >= POOL_DEVICE_MAX) return false;
    out = ds.data().pool.metrics[idx];
    return out.valid;
}

static inline bool samePoolDeviceTarget(const PoolDeviceTarget& a, const PoolDeviceTarget& b)
{
    return a.running == b.running && a.setpoint == b.setpoint && a.mode == b.mode;
}

// Read timestamps track freshness. The values, validity and diagnostics drive notifications.
static inline bool samePoolDeviceFeedback(const PoolDeviceFeedback& a, const PoolDeviceFeedback& b)
{
    return samePoolDeviceTarget(a.applied, b.applied) && samePoolDeviceTarget(a.observed, b.observed) &&
           a.appliedValid == b.appliedValid && a.observedValid == b.observedValid &&
           a.online == b.online && a.quality == b.quality && a.phase == b.phase &&
           a.appliedRevision == b.appliedRevision && a.error == b.error &&
           a.communicationDegraded == b.communicationDegraded &&
           a.communicationFresh == b.communicationFresh && a.communicationLost == b.communicationLost &&
           a.hasObservation == b.hasObservation && a.deviceFaults == b.deviceFaults &&
           a.telemetry.validBlocks == b.telemetry.validBlocks && a.telemetry.error == b.telemetry.error &&
           memcmp(a.telemetry.raw, b.telemetry.raw, sizeof(a.telemetry.raw)) == 0;
}

static inline bool samePoolDeviceState(const PoolDeviceRuntimeStateEntry& a, const PoolDeviceRuntimeStateEntry& b)
{
    return a.valid == b.valid && a.enabled == b.enabled &&
           sameActuatorControl(a.control, b.control) &&
           a.desiredOn == b.desiredOn && a.actualOn == b.actualOn && a.type == b.type &&
           a.interlockState == b.interlockState && a.blockReason == b.blockReason &&
           a.desiredSetpoint == b.desiredSetpoint && a.effectiveOn == b.effectiveOn &&
           a.effectiveSetpoint == b.effectiveSetpoint && a.actualOnSinceMs == b.actualOnSinceMs &&
           samePoolDeviceFeedback(a.feedback, b.feedback);
}

static inline bool setPoolDeviceRuntimeState(DataStore& ds, uint8_t idx,
                                             const PoolDeviceRuntimeStateEntry& in, bool refreshCountdown)
{
    if (idx >= POOL_DEVICE_MAX) return false;

    RuntimeData& rt = ds.dataMutable();
    PoolDeviceRuntimeStateEntry& cur = rt.pool.state[idx];
    const bool changed = !samePoolDeviceState(cur, in) || refreshCountdown;
    const bool overridePanelChanged = (cur.control.supported || in.control.supported) &&
        (cur.actualOn != in.actualOn || cur.desiredOn != in.desiredOn);
    const uint32_t sequence = cur.sequence + uint32_t(changed);
    const uint32_t tsMs = changed ? in.tsMs : cur.tsMs;

    cur = in;
    cur.sequence = sequence;
    cur.tsMs = tsMs;
    if (changed) ds.notifyChanged(DATAKEY_POOL_DEVICE_STATE_BASE + idx);
    if (overridePanelChanged) ds.notifyChanged(DataKeys::PoolDeviceOverrides);
    return changed;
}

static inline bool setPoolDeviceRuntimeMetrics(DataStore& ds, uint8_t idx,
                                               const PoolDeviceRuntimeMetricsEntry& in)
{
    if (idx >= POOL_DEVICE_MAX) return false;

    RuntimeData& rt = ds.dataMutable();
    PoolDeviceRuntimeMetricsEntry& cur = rt.pool.metrics[idx];
    if (memcmp(&cur, &in, sizeof(PoolDeviceRuntimeMetricsEntry)) == 0) return false;

    cur = in;
    ds.notifyChanged((DataKey)(DATAKEY_POOL_DEVICE_METRICS_BASE + idx));
    return true;
}

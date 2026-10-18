#pragma once
/**
 * @file IPoolDevice.h
 * @brief Pool device domain service interface.
 */
#include <stdint.h>
#include "Core/Services/PoolInterlockState.h"
#include "IIO.h"
#include "PoolActuatorTypes.h"
#include "ActuatorControlState.h"
#include "Domain/DomainTypes.h"

struct CommandRequest;

/** Result code for PoolDeviceService calls. */
enum PoolDeviceSvcStatus : uint8_t {
    POOLDEV_SVC_OK = 0,
    POOLDEV_SVC_ERR_INVALID_ARG = 1,
    POOLDEV_SVC_ERR_UNKNOWN_SLOT = 2,
    POOLDEV_SVC_ERR_NOT_READY = 3,
    POOLDEV_SVC_ERR_DISABLED = 4,
    POOLDEV_SVC_ERR_INTERLOCK = 5,
    POOLDEV_SVC_ERR_IO = 6,
    POOLDEV_SVC_ERR_MAX_UPTIME = 7,
    POOLDEV_SVC_ERR_WRITES_DISABLED = 8
};

/** Static metadata for one pool device slot. */
struct PoolDeviceSvcMeta {
    uint8_t slot = 0;
    uint8_t used = 0;
    uint8_t type = 0;
    uint8_t enabled = 0;
    PoolInterlockState interlockState = PoolInterlockState::Ready;
    uint8_t blockReason = 0;
    IoId ioId = IO_ID_INVALID;
    PoolDeviceCapabilities capabilities{};
    bool driverReady = false;
    bool guidedOn = false;
    PoolDeviceTarget guidedTarget{};
    PoolRunModes runModes{};
    PoolTelemetryProfile telemetryProfile = PoolTelemetryProfile::None;
    uint8_t outputCount = 0;
    IoId outputs[POOL_MAX_SPEED_STEPS]{};
    /** Domain actuator associated with this equipment, used for presentation. */
    DomainSlotId commandSlot = DOMAIN_SLOT_INVALID;
    char runtimeId[8] = {0};
    char label[24] = {0};
    /** Configured nominal device flow, in litres per hour; zero means unknown. */
    ActuatorControlState control{};
    float flowLPerHour = 0.0f;
};

/** Service interface for slot-based pool device control. */
struct PoolDeviceService {
    /** Number of active pool-device slots. */
    uint8_t (*count)(void* ctx);
    /** Metadata lookup for one slot index. */
    PoolDeviceSvcStatus (*meta)(void* ctx, uint8_t slot, PoolDeviceSvcMeta* outMeta);
    /** Read actual hardware state of one slot. */
    PoolDeviceSvcStatus (*readActualOn)(void* ctx, uint8_t slot, uint8_t* outOn, uint32_t* outTsMs);
    /** Write desired state of one slot. */
    PoolDeviceSvcStatus (*setRunning)(void* ctx, uint8_t slot, uint8_t on);
    /** Enable or freeze physical actuator writes while keeping runtime readable. */
    PoolDeviceSvcStatus (*setWritesEnabled)(void* ctx, uint8_t enabled);
    /** Read whether physical actuator writes are currently enabled. */
    uint8_t (*writesEnabled)(void* ctx);
    /** Refill tracked tank level for one slot (peristaltic pumps). */
    PoolDeviceSvcStatus (*refillTank)(void* ctx, uint8_t slot, float remainingL);
    PoolDeviceSvcStatus (*setTarget)(void* ctx, uint8_t slot, const PoolDeviceTarget* target);
    PoolDeviceSvcStatus (*readState)(void* ctx, uint8_t slot, PoolDeviceFeedback* state);
    /** Opaque implementation context. */
    void* ctx;
    /** Replace the controller safety policy atomically; unspecified slots use default policy. */
    PoolDeviceSvcStatus (*setOverridePolicies)(void* ctx, const ActuatorOverridePolicy* policies, uint8_t count) = nullptr;
    PoolDeviceSvcStatus (*releaseOverride)(void* ctx, uint8_t slot, ActuatorOverrideReason reason) = nullptr;
    /** Compatibility adapters forward a command without duplicating lease ownership. */
    bool (*overrideCommand)(void* ctx, const CommandRequest& request, char* reply, size_t length, ActuatorOverrideCommand action) = nullptr;
    uint16_t (*overrideDuration)(void* ctx) = nullptr;
    /** Apply an unlimited manual target and release its lease in one transaction. */
    PoolDeviceSvcStatus (*setManualRunning)(void* ctx, uint8_t slot, uint8_t on) = nullptr;
    /** Atomically change running/setpoint while preserving the currently selected operating mode. */
    PoolDeviceSvcStatus (*setRunningAtSetpoint)(void* ctx, uint8_t slot, uint8_t on, float setpoint) = nullptr;
};

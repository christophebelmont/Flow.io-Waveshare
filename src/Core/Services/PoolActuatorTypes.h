#pragma once
#include "IIO.h"
#include "IModbusMaster.h"

constexpr uint8_t POOL_MAX_SPEED_STEPS = 6;
enum class PoolControlKind : uint8_t { Relay, Discrete, Analog, Rs485 };
enum class PoolSetpointUnit : uint8_t { SpeedPercent, PowerPercent, Celsius };
enum class PoolFeedbackQuality : uint8_t { Unknown, Estimated, Confirmed, Stale };
enum class PoolCommandPhase : uint8_t { Idle, Pending, Applied, Failed, Frozen };

struct PoolDeviceTarget {
    bool running = false;
    float setpoint = 100;
};
struct PoolDeviceFeedback {
    PoolDeviceTarget applied{};
    PoolDeviceTarget observed{};
    bool appliedValid = false;
    bool observedValid = false;
    bool online = false;
    PoolFeedbackQuality quality = PoolFeedbackQuality::Unknown;
    PoolCommandPhase phase = PoolCommandPhase::Idle;
    uint32_t appliedRevision = 0;
    uint32_t observedAtMs = 0;
    uint32_t changedAtMs = 0;
    uint16_t error = 0;
};
struct PoolDeviceCapabilities {
    PoolControlKind kind = PoolControlKind::Relay;
    PoolSetpointUnit unit = PoolSetpointUnit::SpeedPercent;
    float minimum = 0;
    float maximum = 100;
    float startup = 100;
    uint8_t stepCount = 0;
    float steps[POOL_MAX_SPEED_STEPS]{};
};

/** Register operations describe the wire format as well as its function byte. */
struct PoolRegisterOperation {
    uint16_t address = 0;
    uint8_t function = 3;
    RegisterOperation operation = RegisterOperation::Read;
};
struct PoolSerialConfig {
    SerialLineProfile line{};
    RegisterWireProtocol protocol = RegisterWireProtocol::ModbusRtu;
    uint8_t address = 1;
    PoolRegisterOperation run{0, 6, RegisterOperation::WriteSingle};
    PoolRegisterOperation setpoint{1, 6, RegisterOperation::WriteSingle};
    PoolRegisterOperation status{0, 3, RegisterOperation::Read};
    PoolRegisterOperation feedback{1, 3, RegisterOperation::Read};
    bool hasFeedback = false;
    uint16_t runValue = 1;
    uint16_t stopValue = 0;
    uint16_t runningMask = 1;
    float rawPerUnit = 1;
    float rawOffset = 0;
    float feedbackUnitsPerRaw = 1;
    float feedbackOffset = 0;
    uint32_t pollMs = 1000;
    uint32_t timeoutMs = 300;
    uint32_t staleMs = 5000;
    uint8_t retries = 1;
};
struct PoolFlowPoint { float setpoint = 0; float litresPerHour = 0; };
struct PoolDriverConfig {
    PoolDeviceCapabilities capabilities{};
    IoId outputs[POOL_MAX_SPEED_STEPS] = {IO_ID_INVALID, IO_ID_INVALID, IO_ID_INVALID, IO_ID_INVALID, IO_ID_INVALID, IO_ID_INVALID};
    uint32_t breakBeforeMakeMs = 250;
    float analogOff = 0;
    float analogGain = 0.1f;
    float analogOffset = 0;
    // Minimum applied/observed level required of each dependency. Zero disables level check.
    float dependencyMinimum = 0;
    bool requireConfirmedDependency = false;
    uint8_t flowPointCount = 0;
    PoolFlowPoint flowPoints[POOL_MAX_SPEED_STEPS]{};
    PoolSerialConfig serial{};
};

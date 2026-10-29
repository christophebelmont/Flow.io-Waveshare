#pragma once
#include "IIO.h"
#include "IModbusMaster.h"
#include "PoolSerialTelemetry.h"

constexpr uint8_t POOL_MAX_SPEED_STEPS = 6;
constexpr uint8_t POOL_MAX_RUN_MODES = 8;
enum class PoolControlKind : uint8_t { Relay, Discrete, Analog, Rs485 };
enum class PoolSetpointUnit : uint8_t { SpeedPercent, PowerPercent, Celsius, Rpm };
inline const char* poolSetpointUnitSymbol(PoolSetpointUnit unit)
{
    switch (unit) {
        case PoolSetpointUnit::SpeedPercent:
        case PoolSetpointUnit::PowerPercent: return "%";
        case PoolSetpointUnit::Celsius: return "°C";
        case PoolSetpointUnit::Rpm: return "rpm";
    }
    return "";
}
enum class PoolSerialModel : uint8_t { Custom, AquaGem, Polytropic };
enum class PoolSerialControl : uint8_t { SeparateRun, SetpointOrStop };
enum class PoolRunningSource : uint8_t { StatusMask, FeedbackThreshold, DeviceStatus };
enum class PoolRegisterValueType : uint8_t { Unsigned16, Signed16 };
enum class PoolRawRounding : uint8_t { Nearest, Down };
enum class PoolFeedbackQuality : uint8_t { Unknown, Estimated, Confirmed, Stale };
enum class PoolCommandPhase : uint8_t { Idle, Pending, Applied, Failed, Frozen, Confirming };
// Confirming: the device acknowledged the command, but fresh feedback has not
// yet reached its target. Feedback quality describes the reading, not the command.

struct PoolDeviceTarget {
    bool running = false;
    float setpoint = 100;
    uint8_t mode = 0;
};
struct PoolRunMode {
    char label[24]{};
    uint16_t value = 0;
};
struct PoolRunModes {
    uint8_t count = 0;
    PoolRunMode options[POOL_MAX_RUN_MODES]{};
};
struct PoolDeviceFeedback {
    // For serial devices, this is the wire-normalized, acknowledged target.
    // With feedback enabled, phase == Applied requires fresh matching feedback.
    // Without feedback, quality remains Estimated after acknowledgement.
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
    uint16_t deviceFaults = 0;
    PoolTelemetryState telemetry{};
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
    RegisterResponseLayout responseLayout = RegisterResponseLayout::Standard;
};
struct PoolSerialConfig {
    PoolSerialModel model = PoolSerialModel::Custom;
    SerialLineProfile line{};
    RegisterWireProtocol protocol = RegisterWireProtocol::ModbusRtu;
    uint8_t address = 1;
    PoolRegisterOperation run{0, 6, RegisterOperation::WriteSingle};
    PoolRegisterOperation setpoint{1, 6, RegisterOperation::WriteSingle};
    PoolRegisterOperation status{0, 3, RegisterOperation::Read};
    PoolRegisterOperation feedback{1, 3, RegisterOperation::Read};
    bool hasFeedback = false;
    PoolSerialControl control = PoolSerialControl::SeparateRun;
    PoolRunningSource runningSource = PoolRunningSource::StatusMask;
    PoolRegisterValueType feedbackType = PoolRegisterValueType::Unsigned16;
    // Threshold is expressed in decoded feedback units; running means strictly greater.
    float runningThreshold = 0;
    uint16_t runValue = 1;
    PoolRunModes modes{};
    PoolTelemetryProfile telemetryProfile = PoolTelemetryProfile::None;
    uint32_t telemetryStaleMs = 30000;
    uint16_t stopValue = 0;
    uint16_t runningMask = 1;
    float rawPerUnit = 1;
    float rawOffset = 0;
    uint16_t rawStep = 1;
    PoolRawRounding rawRounding = PoolRawRounding::Nearest;
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

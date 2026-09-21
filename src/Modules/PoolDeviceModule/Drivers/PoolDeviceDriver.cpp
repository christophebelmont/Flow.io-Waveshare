#include "PoolDeviceDriver.h"
#include "Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.h"
#include <cmath>

namespace {
bool due(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
}

bool validatePoolTarget(const PoolDriverConfig& c, const PoolDeviceTarget& t)
{
    if (!std::isfinite(t.setpoint) || c.capabilities.stepCount > POOL_MAX_SPEED_STEPS) return false;
    const auto& caps = c.capabilities;
    if (caps.kind == PoolControlKind::Relay) return t.setpoint == caps.startup;
    if (t.setpoint < caps.minimum || t.setpoint > caps.maximum) return false;
    if (caps.kind == PoolControlKind::Discrete) {
        for (uint8_t i = 0; i < caps.stepCount; ++i) if (t.setpoint == caps.steps[i]) return true;
        return false;
    }
    return true;
}

bool validatePoolDriverConfig(const PoolDriverConfig& c)
{
    if (c.flowPointCount > POOL_MAX_SPEED_STEPS) return false;
    for (uint8_t i = 0; i < c.flowPointCount; ++i) {
        const auto& point = c.flowPoints[i];
        if (!std::isfinite(point.setpoint) || !std::isfinite(point.litresPerHour) || point.litresPerHour < 0 ||
            (i && point.setpoint <= c.flowPoints[i-1].setpoint)) return false;
    }
    const auto& p = c.capabilities;
    if (uint8_t(p.kind) > 3 || uint8_t(p.unit) > 2 || !std::isfinite(p.minimum) ||
        !std::isfinite(p.maximum) || p.minimum > p.maximum ||
        !std::isfinite(c.dependencyMinimum) || c.breakBeforeMakeMs > 60000 ||
        !validatePoolTarget(c, {true, p.startup})) return false;
    if (p.unit != PoolSetpointUnit::Celsius && (p.minimum < 0 || p.maximum > 100)) return false;
    const uint8_t count = p.kind == PoolControlKind::Discrete ? p.stepCount : 1;
    if (p.kind != PoolControlKind::Rs485) {
        if (!count || count > POOL_MAX_SPEED_STEPS) return false;
        for (uint8_t i = 0; i < count; ++i) {
            if (c.outputs[i] == IO_ID_INVALID) return false;
            for (uint8_t j = 0; j < i; ++j) if (c.outputs[j] == c.outputs[i]) return false;
            if (p.kind == PoolControlKind::Discrete &&
                (!std::isfinite(p.steps[i]) || p.steps[i] < p.minimum || p.steps[i] > p.maximum ||
                 (i && p.steps[i] <= p.steps[i-1]))) return false;
        }
    }
    if (p.kind == PoolControlKind::Analog)
        return std::isfinite(c.analogGain) && c.analogGain != 0 &&
            std::isfinite(c.analogOffset) && std::isfinite(c.analogOff) &&
            std::isfinite(p.minimum * c.analogGain + c.analogOffset) &&
            std::isfinite(p.maximum * c.analogGain + c.analogOffset);
    if (p.kind != PoolControlKind::Rs485) return true;
    const auto& s = c.serial;
    if (!std::isfinite(s.rawPerUnit) || s.rawPerUnit <= 0 || !std::isfinite(s.rawOffset) ||
        !std::isfinite(s.feedbackUnitsPerRaw) || s.feedbackUnitsPerRaw <= 0 ||
        !std::isfinite(s.feedbackOffset) || !std::isfinite(65535.0f * s.feedbackUnitsPerRaw + s.feedbackOffset) ||
        s.runValue == s.stopValue || !s.runningMask ||
        s.line.busId != 0 || s.line.baud < 1200 || s.line.baud > 115200 || s.line.parity > 2 ||
        (s.line.stopBits != 1 && s.line.stopBits != 2) || s.line.quietMs > 1000 ||
        s.line.lateResponseGuardMs > 5000 || s.timeoutMs < 10 || s.timeoutMs > 5000 || s.retries > 3 ||
        s.pollMs < 50 || s.pollMs > 60000 || s.staleMs < 2 * s.pollMs + s.timeoutMs || s.staleMs > 300000)
        return false;
    if (p.minimum * s.rawPerUnit + s.rawOffset < 0 ||
        p.maximum * s.rawPerUnit + s.rawOffset > 65535) return false;
    const PoolRegisterOperation operations[] = {s.run, s.setpoint, s.status, s.feedback};
    for (uint8_t i = 0; i < (s.hasFeedback ? 4 : 2); ++i) {
        ModbusRequest r{};
        r.slaveAddress = s.address; r.protocol = s.protocol;
        r.function = operations[i].function; r.operation = operations[i].operation;
        if (!ModbusRtuCodec::validRequest(r) ||
            (i < 2 ? ModbusRtuCodec::operation(r) == RegisterOperation::Read :
                     ModbusRtuCodec::operation(r) != RegisterOperation::Read)) return false;
    }
    return true;
}

bool PoolDriverBase::begin(const PoolDriverConfig& config, const IOServiceV2* io,
                           const ModbusMasterService* bus, uint8_t owner)
{
    if (!owner || !validatePoolDriverConfig(config)) return false;
    config_ = config; io_ = io; bus_ = bus; owner_ = owner;
    target_ = {false, config.capabilities.startup};
    return config.capabilities.kind == PoolControlKind::Rs485 ?
        bus && bus->submit && bus->poll && bus->cancelOwner :
        io && io->runtimeStatus && io->readDigital && io->readValue && io->writeDigital && io->writeAnalog;
}

void PoolDriverBase::applyTarget(const PoolDeviceTarget& t, uint32_t revision)
{
    if (revision_ == revision) return;
    target_ = t; revision_ = revision;
    state_.phase = PoolCommandPhase::Pending;
    retryAt_ = 0;
}

bool PoolDriverBase::writable_(bool enabled)
{
    if (!enabled) { state_.phase = PoolCommandPhase::Frozen; return false; }
    return true;
}

bool PoolDriverBase::outputsAvailable_(uint32_t now)
{
    const uint8_t count = config_.capabilities.kind == PoolControlKind::Discrete ?
        config_.capabilities.stepCount : 1;
    for (uint8_t i = 0; i < count; ++i) {
        IoRuntimeStatus runtime{};
        const IoStatus result = io_->runtimeStatus(io_->ctx, config_.outputs[i], &runtime);
        if (result == IO_OK && runtime.state == IO_RUNTIME_ACTIVE) continue;
        const IoStatus error = result != IO_OK ? result :
            (runtime.state == IO_RUNTIME_MANUALLY_DISABLED ? IO_ERR_DISABLED : IO_ERR_NOT_READY);
        state_.appliedValid = false;
        failed_(error, now);
        return false;
    }
    return true;
}

void PoolDriverBase::applied_(const PoolDeviceTarget& t, uint32_t revision, uint32_t now)
{
    state_.applied = t;
    state_.appliedValid = true;
    state_.appliedRevision = revision;
    state_.online = true;
    state_.phase = revision == revision_ ? PoolCommandPhase::Applied : PoolCommandPhase::Pending;
    state_.error = 0;
    state_.changedAtMs = now;
    // A physical output or acknowledgement is evidence of the command, not of rotation.
    if (!config_.serial.hasFeedback || config_.capabilities.kind != PoolControlKind::Rs485) {
        state_.observed = t;
        state_.observedValid = true;
        state_.quality = PoolFeedbackQuality::Estimated;
        state_.observedAtMs = now;
    }
}
void PoolDriverBase::failed_(uint16_t error, uint32_t now)
{
    state_.error = error; state_.phase = PoolCommandPhase::Failed;
    state_.appliedValid = false;
    state_.online = false; state_.observedValid = false;
    state_.quality = PoolFeedbackQuality::Unknown; state_.changedAtMs = now;
    retryAt_ = now + 1000;
}
bool PoolDriverBase::writeDigital_(IoId id, bool on, uint32_t now)
{
    return io_->writeDigital(io_->ctx, id, on, now, owner_) == IO_OK;
}
void DigitalRelayDriver::tick(uint32_t now, bool enabled)
{
    if (!outputsAvailable_(now)) return;
    if (!writable_(enabled) || !due(now, retryAt_)) return;
    uint8_t on = 0;
    if (state_.appliedValid && state_.appliedRevision == revision_ &&
        io_->readDigital(io_->ctx, config_.outputs[0], &on, nullptr, nullptr) == IO_OK &&
        (on != 0) == target_.running) { state_.phase = PoolCommandPhase::Applied; return; }
    if (writeDigital_(config_.outputs[0], target_.running, now)) applied_(target_, revision_, now);
    else failed_(IO_ERR_HW, now);
}
void DiscreteSpeedDriver::tick(uint32_t now, bool enabled)
{
    if (!outputsAvailable_(now)) { cleared_ = false; return; }
    if (!writable_(enabled) || !due(now, retryAt_)) return;
    if (!cleared_ && state_.appliedValid && state_.appliedRevision == revision_) {
        bool matches = true;
        for (uint8_t i = 0; i < config_.capabilities.stepCount; ++i) {
            uint8_t on = 0;
            const IoStatus result = io_->readDigital(io_->ctx, config_.outputs[i], &on, nullptr, nullptr);
            if (result != IO_OK) { failed_(result, now); return; }
            const bool expected = target_.running && target_.setpoint == config_.capabilities.steps[i];
            matches = matches && ((on != 0) == expected);
        }
        if (matches) { state_.phase = PoolCommandPhase::Applied; return; }
        // Any output mismatch requires a new all-off interval before energising a speed.
        cleared_ = false;
    }
    if (!cleared_) {
        bool ok = true;
        for (uint8_t i = 0; i < config_.capabilities.stepCount; ++i)
            if (!writeDigital_(config_.outputs[i], false, now)) ok = false;
        if (!ok) { failed_(IO_ERR_HW, now); return; }
        state_.applied = {false, target_.setpoint}; state_.appliedValid = true;
        state_.observed = state_.applied; state_.observedValid = true;
        state_.quality = PoolFeedbackQuality::Estimated;
        state_.observedAtMs = now; state_.changedAtMs = now;
        cleared_ = true; readyAt_ = now + config_.breakBeforeMakeMs;
    }
    if (!target_.running) { applied_(target_, revision_, now); cleared_ = false; return; }
    if (!due(now, readyAt_)) return;
    for (uint8_t i = 0; i < config_.capabilities.stepCount; ++i) {
        if (target_.setpoint != config_.capabilities.steps[i]) continue;
        if (writeDigital_(config_.outputs[i], true, now)) applied_(target_, revision_, now);
        else failed_(IO_ERR_HW, now);
        cleared_ = false;
        return;
    }
    failed_(IO_ERR_INVALID_ARG, now);
}
void AnalogSetpointDriver::tick(uint32_t now, bool enabled)
{
    if (!outputsAvailable_(now)) return;
    if (!writable_(enabled) || !due(now, retryAt_)) return;
    const float value = target_.running ? target_.setpoint * config_.analogGain + config_.analogOffset : config_.analogOff;
    if (state_.appliedValid && state_.appliedRevision == revision_) {
        IoValue current{};
        if (io_->readValue(io_->ctx, config_.outputs[0], &current) == IO_OK &&
            current.valid && current.type == IO_VAL_FLOAT && current.v.f == value) {
            state_.phase = PoolCommandPhase::Applied; return;
        }
    }
    const IoStatus result = io_->writeAnalog(io_->ctx, config_.outputs[0], value, now, owner_);
    if (result == IO_OK) applied_(target_, revision_, now);
    else failed_(result, now);
}

void SerialDeviceDriver::applyTarget(const PoolDeviceTarget& t, uint32_t revision)
{
    if (revision == revision_) return;
    // Drop queued obsolete operations. The arbiter drains any active exchange before proceeding.
    if (transaction_ != MODBUS_TRANSACTION_INVALID) {
        bus_->cancelOwner(bus_->ctx, owner_);
        transaction_ = MODBUS_TRANSACTION_INVALID;
        operation_ = Operation::None;
    }
    PoolDriverBase::applyTarget(t, revision);
}

bool SerialDeviceDriver::submit_(Operation op, uint32_t now)
{
    const auto& c = config_.serial;
    const PoolRegisterOperation* wire = nullptr;
    switch (op) {
        case Operation::Setpoint: wire = &c.setpoint; break;
        case Operation::Run: wire = &c.run; break;
        case Operation::Status: wire = &c.status; break;
        case Operation::Feedback: wire = &c.feedback; break;
        default: return false;
    }
    ModbusRequest r{};
    r.ownerId = owner_; r.slaveAddress = c.address; r.line = c.line; r.protocol = c.protocol;
    r.function = wire->function; r.operation = wire->operation; r.registerAddress = wire->address;
    r.responseTimeoutMs = c.timeoutMs; r.retries = c.retries;
    r.priority = op == Operation::Run && !target_.running ? MODBUS_PRIORITY_SAFETY :
        (op == Operation::Run || op == Operation::Setpoint ? MODBUS_PRIORITY_COMMAND : MODBUS_PRIORITY_BACKGROUND);
    if (op == Operation::Run) r.values[0] = target_.running ? c.runValue : c.stopValue;
    if (op == Operation::Setpoint) r.values[0] = uint16_t(std::lround(target_.setpoint * c.rawPerUnit + c.rawOffset));
    const auto result = bus_->submit(bus_->ctx, &r, &transaction_);
    if (result != MODBUS_RESULT_OK) {
        transaction_ = MODBUS_TRANSACTION_INVALID;
        if (result != MODBUS_RESULT_QUEUE_FULL) failed_(result, now);
        return false;
    }
    operation_ = op; sentTarget_ = target_; sentRevision_ = revision_;
    return true;
}
void SerialDeviceDriver::consume_(const ModbusResponse& r, uint32_t now)
{
    if (r.result != MODBUS_RESULT_OK) {
        statusValid_ = levelValid_ = false; failed_(r.result, now); return;
    }
    state_.online = true; state_.error = 0;
    switch (operation_) {
        case Operation::Setpoint: speedRevision_ = sentRevision_; break;
        case Operation::Run: applied_(sentTarget_, sentRevision_, now); nextPollAt_ = now; break;
        case Operation::Status:
            state_.observed.running = (r.values[0] & config_.serial.runningMask) != 0;
            statusAt_ = now; statusValid_ = true; break;
        case Operation::Feedback:
            state_.observed.setpoint = r.values[0] * config_.serial.feedbackUnitsPerRaw + config_.serial.feedbackOffset;
            levelAt_ = now; levelValid_ = true; break;
        default: break;
    }
    state_.changedAtMs = now;
}
void SerialDeviceDriver::tick(uint32_t now, bool enabled)
{
    if (!enabled && !frozen_) {
        bus_->cancelOwner(bus_->ctx, owner_); transaction_ = MODBUS_TRANSACTION_INVALID;
    }
    frozen_ = !enabled;
    if (transaction_ != MODBUS_TRANSACTION_INVALID) {
        ModbusResponse r{};
        const auto result = bus_->poll(bus_->ctx, transaction_, &r);
        if (result != MODBUS_RESULT_NOT_READY) {
            if (result == MODBUS_RESULT_OK) consume_(r, now);
            else { statusValid_ = levelValid_ = false; failed_(result, now); }
            transaction_ = MODBUS_TRANSACTION_INVALID; operation_ = Operation::None;
        }
    }
    if (config_.serial.hasFeedback) {
        state_.observedValid = statusValid_ && levelValid_ &&
            uint32_t(now - statusAt_) <= config_.serial.staleMs && uint32_t(now - levelAt_) <= config_.serial.staleMs;
        state_.quality = state_.observedValid ? PoolFeedbackQuality::Confirmed :
            (statusValid_ || levelValid_ ? PoolFeedbackQuality::Stale : PoolFeedbackQuality::Unknown);
        state_.observedAtMs = uint32_t(now - statusAt_) > uint32_t(now - levelAt_) ? statusAt_ : levelAt_;
    }
    if (!config_.serial.hasFeedback && uint32_t(now - state_.observedAtMs) >= config_.serial.staleMs) {
        state_.observedValid = false; state_.quality = PoolFeedbackQuality::Stale;
    }
    if (!writable_(enabled) || transaction_ != MODBUS_TRANSACTION_INVALID || !due(now, retryAt_)) return;
    if (!state_.appliedValid || state_.appliedRevision != revision_) {
        submit_(target_.running && speedRevision_ != revision_ ? Operation::Setpoint : Operation::Run, now);
        return;
    }
    state_.phase = PoolCommandPhase::Applied;
    if (!config_.serial.hasFeedback && uint32_t(now - state_.observedAtMs) >= config_.serial.pollMs) {
        // Explicit idempotent register writes refresh devices without readback.
        submit_(Operation::Run, now);
    } else if (config_.serial.hasFeedback && due(now, nextPollAt_)) {
        if (submit_(pollLevel_ ? Operation::Feedback : Operation::Status, now)) {
            pollLevel_ = !pollLevel_; nextPollAt_ = now + config_.serial.pollMs;
        }
    }
}
IPoolDeviceDriver& PoolDeviceDriver::configure(PoolControlKind kind)
{
    switch (kind) {
        case PoolControlKind::Relay: driver_.emplace<DigitalRelayDriver>(); break;
        case PoolControlKind::Discrete: driver_.emplace<DiscreteSpeedDriver>(); break;
        case PoolControlKind::Analog: driver_.emplace<AnalogSetpointDriver>(); break;
        case PoolControlKind::Rs485: driver_.emplace<SerialDeviceDriver>(); break;
    }
    return get();
}
IPoolDeviceDriver& PoolDeviceDriver::get()
{
    return std::visit([](auto& driver) -> IPoolDeviceDriver& { return driver; }, driver_);
}

float poolCalibratedFlow(const PoolDriverConfig& c, float value)
{
    if (!c.flowPointCount || !std::isfinite(value)) return NAN;
    if (value < c.flowPoints[0].setpoint || value > c.flowPoints[c.flowPointCount-1].setpoint) return NAN;
    for (uint8_t i = 0; i < c.flowPointCount; ++i) {
        const auto& point = c.flowPoints[i];
        if (value == point.setpoint) return point.litresPerHour;
        if (value < point.setpoint && i) {
            const auto& prev = c.flowPoints[i-1];
            const float ratio = (value - prev.setpoint) / (point.setpoint - prev.setpoint);
            return prev.litresPerHour + ratio * (point.litresPerHour - prev.litresPerHour);
        }
    }
    return NAN;
}

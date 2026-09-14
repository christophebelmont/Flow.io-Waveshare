/**
 * @file ModbusVariableSpeedPumpDriver.cpp
 * @brief Descriptor-driven variable-speed pump driver implementation.
 */

#include "ModbusVariableSpeedPumpDriver.h"

#include <math.h>

void ModbusVariableSpeedPumpDriver::configure(const ModbusVariableSpeedPumpConfig& config)
{
    config_ = config;
}

bool ModbusVariableSpeedPumpDriver::begin(const ModbusMasterService* modbus)
{
    modbus_ = modbus;
    const bool validReadFunction =
        config_.readFunction == MODBUS_FC_READ_HOLDING_REGISTERS ||
        config_.readFunction == MODBUS_FC_READ_INPUT_REGISTERS;
    const bool hasMappedRegister =
        config_.registers.status != VARIABLE_PUMP_REGISTER_UNUSED ||
        config_.registers.speedCommand != VARIABLE_PUMP_REGISTER_UNUSED ||
        config_.registers.actualSpeed != VARIABLE_PUMP_REGISTER_UNUSED ||
        config_.registers.actualFlow != VARIABLE_PUMP_REGISTER_UNUSED ||
        config_.registers.power != VARIABLE_PUMP_REGISTER_UNUSED ||
        config_.registers.fault != VARIABLE_PUMP_REGISTER_UNUSED ||
        config_.registers.runCommand != VARIABLE_PUMP_REGISTER_UNUSED;
    ready_ = config_.enabled && config_.ownerId != 0U && validReadFunction &&
             hasMappedRegister && config_.pollIntervalMs > 0U &&
             config_.responseTimeoutMs > 0U &&
             config_.minimumSpeedPercent <= config_.maximumSpeedPercent &&
             modbus_ && modbus_->submit && modbus_->poll;
    state_ = VariableSpeedPumpState{};
    transactionId_ = MODBUS_TRANSACTION_INVALID;
    transactionKind_ = TransactionKind::None;
    nextPollKindValue_ = TransactionKind::ReadStatus;
    return ready_;
}

bool ModbusVariableSpeedPumpDriver::readState(VariableSpeedPumpState& outState) const
{
    outState = state_;
    return ready_;
}

VariableSpeedPumpStatus ModbusVariableSpeedPumpDriver::setRunning(bool running)
{
    if (!config_.enabled) return VARIABLE_SPEED_PUMP_DISABLED;
    if (!ready_ || config_.registers.runCommand == VARIABLE_PUMP_REGISTER_UNUSED) {
        return VARIABLE_SPEED_PUMP_NOT_READY;
    }
    pendingRunningValue_ = running;
    pendingRunningWrite_ = true;
    return VARIABLE_SPEED_PUMP_ACCEPTED;
}

VariableSpeedPumpStatus ModbusVariableSpeedPumpDriver::setSpeedPercent(float speedPercent)
{
    if (!isfinite(speedPercent) || speedPercent < config_.minimumSpeedPercent ||
        speedPercent > config_.maximumSpeedPercent) {
        return VARIABLE_SPEED_PUMP_INVALID_ARGUMENT;
    }
    if (!config_.enabled) return VARIABLE_SPEED_PUMP_DISABLED;
    if (!ready_ || config_.registers.speedCommand == VARIABLE_PUMP_REGISTER_UNUSED ||
        config_.registers.speedCommandRawPerPercent <= 0.0f) {
        return VARIABLE_SPEED_PUMP_NOT_READY;
    }
    pendingSpeedPercent_ = speedPercent;
    pendingSpeedWrite_ = true;
    return VARIABLE_SPEED_PUMP_ACCEPTED;
}

uint16_t ModbusVariableSpeedPumpDriver::encodeScaled_(float value, float rawPerUnit)
{
    const float raw = value * rawPerUnit;
    if (raw <= 0.0f) return 0U;
    if (raw >= 65535.0f) return 65535U;
    return (uint16_t)lroundf(raw);
}

bool ModbusVariableSpeedPumpDriver::submit_(const ModbusRequest& request,
                                            TransactionKind kind,
                                            uint32_t nowMs)
{
    uint16_t transactionId = MODBUS_TRANSACTION_INVALID;
    if (modbus_->submit(modbus_->ctx, &request, &transactionId) != MODBUS_RESULT_OK) {
        return false;
    }
    transactionId_ = transactionId;
    transactionKind_ = kind;
    lastSubmitMs_ = nowMs;
    return true;
}

bool ModbusVariableSpeedPumpDriver::submitRead_(TransactionKind kind,
                                                uint16_t address,
                                                uint32_t nowMs)
{
    ModbusRequest request{};
    request.ownerId = config_.ownerId;
    request.slaveAddress = config_.slaveAddress;
    request.function = config_.readFunction;
    request.priority = MODBUS_PRIORITY_BACKGROUND;
    request.registerAddress = address;
    request.registerCount = 1U;
    request.responseTimeoutMs = config_.responseTimeoutMs;
    request.retries = config_.retries;
    return submit_(request, kind, nowMs);
}

bool ModbusVariableSpeedPumpDriver::submitWrite_(TransactionKind kind,
                                                 uint16_t address,
                                                 uint16_t value,
                                                 uint32_t nowMs)
{
    ModbusRequest request{};
    request.ownerId = config_.ownerId;
    request.slaveAddress = config_.slaveAddress;
    request.function = MODBUS_FC_WRITE_SINGLE_REGISTER;
    request.priority = MODBUS_PRIORITY_COMMAND;
    request.registerAddress = address;
    request.registerCount = 1U;
    request.values[0] = value;
    request.responseTimeoutMs = config_.responseTimeoutMs;
    request.retries = config_.retries;
    return submit_(request, kind, nowMs);
}

uint16_t ModbusVariableSpeedPumpDriver::registerForPoll_(TransactionKind kind) const
{
    switch (kind) {
        case TransactionKind::ReadStatus: return config_.registers.status;
        case TransactionKind::ReadActualSpeed: return config_.registers.actualSpeed;
        case TransactionKind::ReadFlow: return config_.registers.actualFlow;
        case TransactionKind::ReadPower: return config_.registers.power;
        case TransactionKind::ReadFault: return config_.registers.fault;
        default: return VARIABLE_PUMP_REGISTER_UNUSED;
    }
}

ModbusVariableSpeedPumpDriver::TransactionKind
ModbusVariableSpeedPumpDriver::nextPollKind_(TransactionKind kind)
{
    switch (kind) {
        case TransactionKind::ReadStatus: return TransactionKind::ReadActualSpeed;
        case TransactionKind::ReadActualSpeed: return TransactionKind::ReadFlow;
        case TransactionKind::ReadFlow: return TransactionKind::ReadPower;
        case TransactionKind::ReadPower: return TransactionKind::ReadFault;
        case TransactionKind::ReadFault:
        default:
            return TransactionKind::ReadStatus;
    }
}

bool ModbusVariableSpeedPumpDriver::submitNextPoll_(uint32_t nowMs)
{
    for (uint8_t checked = 0U; checked < 5U; ++checked) {
        const TransactionKind kind = nextPollKindValue_;
        nextPollKindValue_ = nextPollKind_(kind);
        const uint16_t address = registerForPoll_(kind);
        if (address == VARIABLE_PUMP_REGISTER_UNUSED) continue;
        return submitRead_(kind, address, nowMs);
    }
    return false;
}

void ModbusVariableSpeedPumpDriver::consumeResponse_(const ModbusResponse& response,
                                                     uint32_t nowMs)
{
    if (response.state != MODBUS_TRANSACTION_COMPLETE ||
        response.result != MODBUS_RESULT_OK) {
        state_.online = false;
        return;
    }

    state_.online = true;
    state_.valid = true;
    state_.updatedAtMs = nowMs;
    const uint16_t raw = response.registerCount > 0U ? response.values[0] : 0U;
    switch (transactionKind_) {
        case TransactionKind::WriteRunning:
            state_.requestedRunning = pendingRunningValue_;
            break;
        case TransactionKind::WriteSpeed:
            state_.requestedSpeedPercent = pendingSpeedPercent_;
            break;
        case TransactionKind::ReadStatus:
            state_.running = (raw & config_.registers.runningMask) != 0U;
            break;
        case TransactionKind::ReadActualSpeed:
            state_.actualSpeedPercent = raw * config_.registers.actualSpeedPercentPerRaw;
            break;
        case TransactionKind::ReadFlow:
            state_.actualFlowM3h = raw * config_.registers.flowM3hPerRaw;
            break;
        case TransactionKind::ReadPower:
            state_.powerW = raw * config_.registers.powerWPerRaw;
            break;
        case TransactionKind::ReadFault:
            state_.faultCode = raw;
            break;
        case TransactionKind::None:
            break;
    }
}

void ModbusVariableSpeedPumpDriver::tick(uint32_t nowMs)
{
    if (!ready_) return;
    if (transactionId_ != MODBUS_TRANSACTION_INVALID) {
        ModbusResponse response{};
        const ModbusResultCode result = modbus_->poll(
            modbus_->ctx, transactionId_, &response);
        if (result == MODBUS_RESULT_NOT_READY) return;
        if (result == MODBUS_RESULT_OK) consumeResponse_(response, nowMs);
        else state_.online = false;
        transactionId_ = MODBUS_TRANSACTION_INVALID;
        transactionKind_ = TransactionKind::None;
        return;
    }

    if (pendingRunningWrite_) {
        const uint16_t value = pendingRunningValue_ ? config_.registers.runValue
                                                    : config_.registers.stopValue;
        if (submitWrite_(TransactionKind::WriteRunning,
                         config_.registers.runCommand, value, nowMs)) {
            pendingRunningWrite_ = false;
        }
        return;
    }
    if (pendingSpeedWrite_) {
        const uint16_t value = encodeScaled_(pendingSpeedPercent_,
                                             config_.registers.speedCommandRawPerPercent);
        if (submitWrite_(TransactionKind::WriteSpeed,
                         config_.registers.speedCommand, value, nowMs)) {
            pendingSpeedWrite_ = false;
        }
        return;
    }
    if ((uint32_t)(nowMs - lastSubmitMs_) >= config_.pollIntervalMs) {
        (void)submitNextPoll_(nowMs);
    }
}

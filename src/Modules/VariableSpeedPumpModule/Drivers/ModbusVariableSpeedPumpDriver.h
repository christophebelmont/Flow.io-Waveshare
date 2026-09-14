#pragma once
/**
 * @file ModbusVariableSpeedPumpDriver.h
 * @brief Descriptor-driven Modbus driver for variable-speed pumps.
 */

#include "Modules/VariableSpeedPumpModule/Drivers/IVariableSpeedPumpDriver.h"

constexpr uint16_t VARIABLE_PUMP_REGISTER_UNUSED = 0xFFFFU;

struct VariableSpeedPumpRegisterMap {
    uint16_t status = VARIABLE_PUMP_REGISTER_UNUSED;
    uint16_t speedCommand = VARIABLE_PUMP_REGISTER_UNUSED;
    uint16_t actualSpeed = VARIABLE_PUMP_REGISTER_UNUSED;
    uint16_t actualFlow = VARIABLE_PUMP_REGISTER_UNUSED;
    uint16_t power = VARIABLE_PUMP_REGISTER_UNUSED;
    uint16_t fault = VARIABLE_PUMP_REGISTER_UNUSED;
    uint16_t runCommand = VARIABLE_PUMP_REGISTER_UNUSED;

    uint16_t runningMask = 0x0001U;
    uint16_t runValue = 1U;
    uint16_t stopValue = 0U;
    float speedCommandRawPerPercent = 1.0f;
    float actualSpeedPercentPerRaw = 1.0f;
    float flowM3hPerRaw = 1.0f;
    float powerWPerRaw = 1.0f;
};

struct ModbusVariableSpeedPumpConfig {
    uint8_t ownerId = 0U;
    uint8_t slaveAddress = 1U;
    bool enabled = false;
    uint8_t readFunction = MODBUS_FC_READ_HOLDING_REGISTERS;
    uint32_t pollIntervalMs = 1000U;
    uint32_t responseTimeoutMs = 500U;
    uint8_t retries = 1U;
    float minimumSpeedPercent = 0.0f;
    float maximumSpeedPercent = 100.0f;
    VariableSpeedPumpRegisterMap registers{};
};

class ModbusVariableSpeedPumpDriver : public IVariableSpeedPumpDriver {
public:
    ModbusVariableSpeedPumpDriver() = default;

    void configure(const ModbusVariableSpeedPumpConfig& config);
    bool begin(const ModbusMasterService* modbus) override;
    void tick(uint32_t nowMs) override;
    bool readState(VariableSpeedPumpState& outState) const override;
    VariableSpeedPumpStatus setRunning(bool running) override;
    VariableSpeedPumpStatus setSpeedPercent(float speedPercent) override;

private:
    enum class TransactionKind : uint8_t {
        None,
        WriteRunning,
        WriteSpeed,
        ReadStatus,
        ReadActualSpeed,
        ReadFlow,
        ReadPower,
        ReadFault
    };

    bool submitRead_(TransactionKind kind, uint16_t address, uint32_t nowMs);
    bool submitWrite_(TransactionKind kind, uint16_t address, uint16_t value, uint32_t nowMs);
    bool submit_(const ModbusRequest& request, TransactionKind kind, uint32_t nowMs);
    void consumeResponse_(const ModbusResponse& response, uint32_t nowMs);
    bool submitNextPoll_(uint32_t nowMs);
    uint16_t registerForPoll_(TransactionKind kind) const;
    static TransactionKind nextPollKind_(TransactionKind kind);
    static uint16_t encodeScaled_(float value, float rawPerUnit);

    ModbusVariableSpeedPumpConfig config_{};
    const ModbusMasterService* modbus_ = nullptr;
    VariableSpeedPumpState state_{};
    uint16_t transactionId_ = MODBUS_TRANSACTION_INVALID;
    TransactionKind transactionKind_ = TransactionKind::None;
    TransactionKind nextPollKindValue_ = TransactionKind::ReadStatus;
    bool ready_ = false;
    bool pendingRunningWrite_ = false;
    bool pendingRunningValue_ = false;
    bool pendingSpeedWrite_ = false;
    float pendingSpeedPercent_ = 0.0f;
    uint32_t lastSubmitMs_ = 0U;
};

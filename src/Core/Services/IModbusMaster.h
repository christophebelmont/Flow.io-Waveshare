#pragma once
/**
 * @file IModbusMaster.h
 * @brief Asynchronous Modbus RTU master service shared by device modules.
 */

#include <stdint.h>

constexpr uint8_t MODBUS_MAX_REGISTERS_PER_REQUEST = 32U;
constexpr uint16_t MODBUS_TRANSACTION_INVALID = 0U;

enum ModbusFunction : uint8_t {
    MODBUS_FC_READ_HOLDING_REGISTERS = 0x03,
    MODBUS_FC_READ_INPUT_REGISTERS = 0x04,
    MODBUS_FC_WRITE_SINGLE_REGISTER = 0x06,
    MODBUS_FC_WRITE_MULTIPLE_REGISTERS = 0x10
};

enum ModbusPriority : uint8_t {
    MODBUS_PRIORITY_BACKGROUND = 0,
    MODBUS_PRIORITY_NORMAL = 1,
    MODBUS_PRIORITY_COMMAND = 2,
    MODBUS_PRIORITY_SAFETY = 3
};

enum ModbusTransactionState : uint8_t {
    MODBUS_TRANSACTION_QUEUED = 0,
    MODBUS_TRANSACTION_COMPLETE = 1,
    MODBUS_TRANSACTION_FAILED = 2
};

enum ModbusResultCode : uint8_t {
    MODBUS_RESULT_OK = 0,
    MODBUS_RESULT_INVALID_ARGUMENT = 1,
    MODBUS_RESULT_NOT_READY = 2,
    MODBUS_RESULT_QUEUE_FULL = 3,
    MODBUS_RESULT_UNKNOWN_TRANSACTION = 4,
    MODBUS_RESULT_TIMEOUT = 5,
    MODBUS_RESULT_CRC_ERROR = 6,
    MODBUS_RESULT_PROTOCOL_ERROR = 7,
    MODBUS_RESULT_EXCEPTION = 8,
    MODBUS_RESULT_IO_ERROR = 9
};

/** Wire dialect is explicit: vendor register frames do not use Modbus exceptions. */
enum class RegisterWireProtocol : uint8_t { ModbusRtu, VendorRegisterRtu };
enum class RegisterOperation : uint8_t { Read, WriteSingle, WriteMultiple };
struct SerialLineProfile {
    uint8_t busId = 0;
    uint32_t baud = 9600;
    uint8_t parity = 0; // 0 none, 1 even, 2 odd
    uint8_t stopBits = 1;
    uint32_t quietMs = 5;
    uint32_t lateResponseGuardMs = 100;
};

struct ModbusRequest {
    /** Stable non-zero identifier assigned by the consuming module. */
    uint8_t ownerId = 0U;
    SerialLineProfile line{};
    RegisterWireProtocol protocol = RegisterWireProtocol::ModbusRtu;
    // Used only by VendorRegisterRtu: register-address/count read, echoed write.
    RegisterOperation operation = RegisterOperation::Read;
    uint8_t slaveAddress = 1U;
    uint8_t function = MODBUS_FC_READ_HOLDING_REGISTERS;
    uint8_t priority = MODBUS_PRIORITY_NORMAL;
    uint16_t registerAddress = 0U;
    uint16_t registerCount = 1U;
    uint16_t values[MODBUS_MAX_REGISTERS_PER_REQUEST]{};
    uint32_t responseTimeoutMs = 500U;
    uint8_t retries = 1U;
};

struct ModbusResponse {
    uint16_t transactionId = MODBUS_TRANSACTION_INVALID;
    uint8_t state = MODBUS_TRANSACTION_QUEUED;
    uint8_t result = MODBUS_RESULT_NOT_READY;
    uint8_t slaveAddress = 0U;
    uint8_t function = 0U;
    uint8_t exceptionCode = 0U;
    uint16_t registerAddress = 0U;
    uint16_t registerCount = 0U;
    uint16_t values[MODBUS_MAX_REGISTERS_PER_REQUEST]{};
};

struct ModbusMasterStats {
    bool ready = false;
    uint8_t queued = 0U;
    uint32_t submitted = 0U;
    uint32_t completed = 0U;
    uint32_t timeouts = 0U;
    uint32_t retries = 0U;
    uint32_t crcErrors = 0U;
    uint32_t protocolErrors = 0U;
    uint32_t ioErrors = 0U;
};

struct ModbusMasterService {
    ModbusResultCode (*submit)(void* ctx,
                               const ModbusRequest* request,
                               uint16_t* outTransactionId);
    /** Poll a transaction. A completed or failed response is consumed by this call. */
    ModbusResultCode (*poll)(void* ctx,
                             uint16_t transactionId,
                             ModbusResponse* outResponse);
    void (*cancelOwner)(void* ctx, uint8_t ownerId);
    bool (*stats)(void* ctx, ModbusMasterStats* outStats);
    void* ctx;
};

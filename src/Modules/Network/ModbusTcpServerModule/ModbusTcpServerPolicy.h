#pragma once
/**
 * @file ModbusTcpServerPolicy.h
 * @brief Fixed-capacity policy and diagnostics contract for the Flow.io TCP/RTU gateway.
 *
 * No UART access is permitted here. Every accepted request must be submitted
 * through ModbusMasterService (the shared RS485 transaction scheduler).
 */
#include <stdint.h>
#include "Core/Services/IModbusMaster.h"

namespace ModbusTcpServer {
constexpr uint8_t kPortCount = 3U;
constexpr uint8_t kClientsPerPort = 2U;
constexpr uint8_t kMaxClients = kPortCount * kClientsPerPort;
constexpr uint16_t kPorts[kPortCount] = {502U, 503U, 504U};

enum class SerialSource : uint8_t { FlowioDevice, Manual };
enum class ErrorMode : uint8_t { ModbusException, Silent };

struct PortConfig {
    bool enabled = false;
    uint16_t tcpPort = 502U;
    SerialSource serialSource = SerialSource::FlowioDevice;
    // Stable identifier of an existing Flow.io RS485 device, resolved by
    // the owning module before submission; never a raw UART reference.
    uint16_t deviceId = 0U;
    SerialLineProfile manualLine{};
    ErrorMode errorMode = ErrorMode::ModbusException;
    uint32_t transactionTimeoutMs = 3000U;
};

struct PortStats {
    uint8_t activeClients = 0U;
    uint8_t maxActiveClients = 0U;
    uint32_t received = 0U;
    uint32_t completed = 0U;
    uint32_t exceptionsSent = 0U;
    uint32_t silentFailures = 0U;
    uint32_t timeouts = 0U;
    uint32_t crcErrors = 0U;
    uint32_t queueRejected = 0U;
    uint32_t resolverFailures = 0U;
    uint32_t connectionsRejected = 0U;
    // Idle sockets reclaimed only when a new client needs a full port.
    uint32_t idleReclaimed = 0U;
    uint32_t peerClosed = 0U;
    uint32_t keepaliveSetupFailures = 0U;
    uint32_t invalidTcpAdus = 0U;
    uint32_t txWouldBlock = 0U;
    uint32_t txErrors = 0U;
    uint32_t rxErrors = 0U;
    uint32_t tcpBytesReceived = 0U;
    uint32_t tcpBytesSent = 0U;
    uint32_t responseTimeTotalMs = 0U;
    uint32_t responseTimeMaxMs = 0U;
    uint32_t queueWaitMaxMs = 0U;
    uint32_t queueWaitTotalMs = 0U;
    uint32_t rtuResponseMaxMs = 0U;
    uint32_t rtuResponseTotalMs = 0U;
    uint32_t rtuSamples = 0U;
};

inline bool validPortIndex(uint8_t index) { return index < kPortCount; }
inline bool validConfig(uint8_t index, const PortConfig& config) {
    return validPortIndex(index) && config.tcpPort == kPorts[index] &&
        config.transactionTimeoutMs >= 100U &&
        config.transactionTimeoutMs <= 120000U &&
        (config.serialSource == SerialSource::Manual ||
         config.serialSource == SerialSource::FlowioDevice) &&
        (config.errorMode == ErrorMode::ModbusException ||
         config.errorMode == ErrorMode::Silent) &&
        (config.serialSource != SerialSource::Manual ||
         (config.manualLine.busId == 0U &&
          config.manualLine.baud >= 1200U && config.manualLine.baud <= 115200U &&
          config.manualLine.parity <= 2U &&
          (config.manualLine.stopBits == 1U || config.manualLine.stopBits == 2U) &&
          config.manualLine.minimumRequestIntervalMs <= 60000U &&
          config.manualLine.quietMs <= 1000U &&
          config.manualLine.lateResponseGuardMs <= 5000U));
}

/** Returns the gateway exception code for an internal failure.
 * 0x0A = gateway path unavailable; 0x0B = gateway target no response;
 * 0x06 = busy. A real slave exception must be forwarded unchanged.
 */
inline uint8_t exceptionCode(ModbusResultCode result, uint8_t slaveException = 0U) {
    switch (result) {
        case MODBUS_RESULT_EXCEPTION: return slaveException;
        case MODBUS_RESULT_QUEUE_FULL: return 0x06U;
        case MODBUS_RESULT_TIMEOUT:
        case MODBUS_RESULT_EXPIRED: return 0x0BU;
        case MODBUS_RESULT_OK: return 0U;
        default: return 0x0AU;
    }
}
} // namespace ModbusTcpServer

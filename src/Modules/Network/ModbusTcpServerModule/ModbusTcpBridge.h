#pragma once
/**
 * @brief Non-blocking, single-request TCP-to-RTU bridge slot.
 *
 * One instance per accepted TCP connection (six maximum). The caller owns
 * TCP framing, connection lifetime and serial-profile resolution. This class
 * NEVER opens a UART; it uses the shared RS485 sequencer service exclusively.
 */
#include "ModbusTcpAdu.h"
#include "ModbusTcpServerPolicy.h"

namespace ModbusTcpServer {
class BridgeSlot {
public:
    enum class State : uint8_t { Idle, Pending, Ready, Silent };
    explicit BridgeSlot(uint8_t ownerId) : ownerId_(ownerId) {}
    State state() const { return state_; }
    bool busy() const { return state_ == State::Pending; }
    ModbusResultCode resultCode() const { return lastResult_; }
    bool hasSerialTiming() const { return hasSerialTiming_; }
    uint32_t queueWaitMs() const { return queueWaitMs_; }
    uint32_t rtuResponseMs() const { return rtuResponseMs_; }
    uint32_t elapsedMs(uint32_t now) const { return now - startedAtMs_; }

    /** Submit exactly one fresh RTU transaction, never serve cached data.
     * serialLine must already be resolved from the selected device or port.
     * On submission failure, a standard exception or silence is prepared.
     */
    bool submit(const ModbusMasterService& service, const ModbusTcpAdu::Request& tcp,
                const SerialLineProfile& serialLine, ErrorMode mode,
                uint32_t timeoutMs, uint32_t now, uint8_t rtuUnitId = 0U) {
        const uint8_t targetUnit = rtuUnitId ? rtuUnitId : tcp.unitId;
        if (state_ != State::Idle || !service.submit || !service.poll ||
            tcp.unitId > 247U || targetUnit < 1U || targetUnit > 247U || !timeoutMs)
            return false;
        service_ = &service;
        mode_ = mode;
        transactionId_ = tcp.transactionId;
        unitId_ = tcp.unitId;
        function_ = tcp.function;
        requestedAddress_ = tcp.address;
        requestedCount_ = tcp.count;
        requestedValue_ = tcp.value;
        startedAtMs_ = now;
        timeoutMs_ = timeoutMs;
        outputSize_ = 0U;
        lastResult_ = MODBUS_RESULT_NOT_READY;
        hasSerialTiming_ = false;
        queueWaitMs_ = 0U;
        rtuResponseMs_ = 0U;
        ModbusRequest request{};
        request.ownerId = ownerId_;
        request.protocol = RegisterWireProtocol::ModbusRtu;
        request.line = serialLine;
        // Dedicated-device ports target the equipment's RTU address;
        // the TCP response still echoes the client's original MBAP Unit ID.
        request.slaveAddress = targetUnit;
        request.function = tcp.function;
        request.registerAddress = tcp.address;
        request.registerCount = tcp.count;
        request.operation = tcp.function == 0x06U ? RegisterOperation::WriteSingle :
            tcp.function == 0x10U ? RegisterOperation::WriteMultiple : RegisterOperation::Read;
        request.priority = request.operation == RegisterOperation::Read ?
            MODBUS_PRIORITY_NORMAL : MODBUS_PRIORITY_COMMAND;
        // Scheduler limits individual RTU response waits to 5000 ms.
        // The per-port timeout remains the overall queue + RTU budget.
        request.responseTimeoutMs = timeoutMs < 5000U ? timeoutMs : 5000U;
        if (request.responseTimeoutMs < 10U) request.responseTimeoutMs = 10U;
        request.transactionBudgetMs = timeoutMs;
        request.retries = 0U;
        if (tcp.function == 0x06U) request.values[0] = tcp.value;
        if (tcp.function == 0x10U) {
            if (!tcp.pdu || tcp.pduLength != 6U + tcp.count * 2U ||
                tcp.count > MODBUS_MAX_WRITE_REGISTERS) return false;
            for (uint16_t i = 0; i < tcp.count; ++i)
                request.values[i] = ModbusTcpAdu::word(tcp.pdu + 6U + i * 2U);
        }
        uint16_t id = MODBUS_TRANSACTION_INVALID;
        const ModbusResultCode result = service.submit(service.ctx, &request, &id);
        if (result != MODBUS_RESULT_OK || id == MODBUS_TRANSACTION_INVALID) {
            fail_(result == MODBUS_RESULT_OK ? MODBUS_RESULT_IO_ERROR : result);
            return true;
        }
        rtuTransactionId_ = id;
        state_ = State::Pending;
        return true;
    }

    /** Complete a request locally when no serial target can be resolved.
     * Never submit an arbitrary request to the RS485 bus.
     */
    bool reject(const ModbusTcpAdu::Request& tcp, ErrorMode mode,
                ModbusResultCode reason, uint32_t now) {
        if (state_ != State::Idle) return false;
        mode_ = mode;
        transactionId_ = tcp.transactionId;
        unitId_ = tcp.unitId;
        function_ = tcp.function;
        startedAtMs_ = now;
        hasSerialTiming_ = false;
        queueWaitMs_ = 0U;
        rtuResponseMs_ = 0U;
        fail_(reason);
        return true;
    }

    /** Report a Modbus protocol exception without closing the TCP socket. */
    bool rejectException(const ModbusTcpAdu::Request& tcp, ErrorMode mode,
                         uint8_t code, uint32_t now) {
        if (!reject(tcp, mode, MODBUS_RESULT_PROTOCOL_ERROR, now)) return false;
        if (mode == ErrorMode::ModbusException) {
            outputSize_ = ModbusTcpAdu::exception(
                transactionId_, unitId_, function_, code, output_, sizeof(output_));
            state_ = outputSize_ ? State::Ready : State::Silent;
        }
        return true;
    }

    /** Must be called regularly; never blocks for an RTU response. */
    void poll(uint32_t now) {
        if (state_ != State::Pending || !service_) return;
        // End-to-end deadline covers scheduler queueing and the RTU response.
        // Polling a completed scheduler transaction after the deadline must not
        // silently grant an extra TCP timeout interval.
        if (uint32_t(now - startedAtMs_) >= timeoutMs_) {
            if (service_->cancelOwner)
                service_->cancelOwner(service_->ctx, ownerId_);
            fail_(MODBUS_RESULT_TIMEOUT);
            return;
        }
        ModbusResponse result{};
        const ModbusResultCode pollResult =
            service_->poll(service_->ctx, rtuTransactionId_, &result);
        if (pollResult == MODBUS_RESULT_NOT_READY) return;
        if (pollResult != MODBUS_RESULT_OK) { fail_(pollResult); return; }
        hasSerialTiming_ = true;
        queueWaitMs_ = result.queueWaitMs;
        rtuResponseMs_ = result.rtuResponseMs;
        if (result.result != MODBUS_RESULT_OK) {
            fail_(static_cast<ModbusResultCode>(result.result), result.exceptionCode);
            return;
        }
        uint8_t pdu[2U + 2U * MODBUS_MAX_REGISTERS_PER_REQUEST]{};
        size_t pduSize = 0U;
        if (function_ == 0x03U || function_ == 0x04U) {
            if (result.registerCount != requestedCount_ ||
                result.registerCount > MODBUS_MAX_REGISTERS_PER_REQUEST) {
                fail_(MODBUS_RESULT_PROTOCOL_ERROR); return;
            }
            pdu[0] = function_;
            pdu[1] = uint8_t(result.registerCount * 2U);
            for (uint16_t i = 0; i < result.registerCount; ++i)
                ModbusTcpAdu::putWord(pdu + 2U + i * 2U, result.values[i]);
            pduSize = 2U + result.registerCount * 2U;
        } else if (function_ == 0x06U || function_ == 0x10U) {
            if (result.registerAddress != requestedAddress_ ||
                (function_ == 0x06U && result.values[0] != requestedValue_) ||
                (function_ == 0x10U && result.registerCount != requestedCount_)) {
                fail_(MODBUS_RESULT_PROTOCOL_ERROR); return;
            }
            pdu[0] = function_;
            ModbusTcpAdu::putWord(pdu + 1U, result.registerAddress);
            ModbusTcpAdu::putWord(pdu + 3U,
                function_ == 0x06U ? result.values[0] : result.registerCount);
            pduSize = 5U;
        } else { fail_(MODBUS_RESULT_INVALID_ARGUMENT); return; }
        lastResult_ = MODBUS_RESULT_OK;
        outputSize_ = ModbusTcpAdu::response(transactionId_, unitId_,
            pdu, pduSize, output_, sizeof(output_));
        state_ = outputSize_ ? State::Ready : State::Silent;
    }

    const uint8_t* output() const { return state_ == State::Ready ? output_ : nullptr; }
    size_t outputSize() const { return state_ == State::Ready ? outputSize_ : 0U; }
    /** Call after sending the response (or recording a silent failure). */
    void reset() {
        // Connection close while pending must cancel the in-flight request.
        if (state_ == State::Pending && service_ && service_->cancelOwner)
            service_->cancelOwner(service_->ctx, ownerId_);
        state_ = State::Idle;
        service_ = nullptr;
        rtuTransactionId_ = MODBUS_TRANSACTION_INVALID;
        outputSize_ = 0U;
    }
private:
    void fail_(ModbusResultCode result, uint8_t slaveException = 0U) {
        lastResult_ = result;
        outputSize_ = 0U;
        if (mode_ == ErrorMode::ModbusException) {
            const uint8_t code = exceptionCode(result, slaveException);
            if (code) outputSize_ = ModbusTcpAdu::exception(transactionId_, unitId_,
                function_, code, output_, sizeof(output_));
        }
        state_ = outputSize_ ? State::Ready : State::Silent;
    }
    uint8_t ownerId_;
    State state_ = State::Idle;
    const ModbusMasterService* service_ = nullptr;
    ErrorMode mode_ = ErrorMode::ModbusException;
    ModbusResultCode lastResult_ = MODBUS_RESULT_NOT_READY;
    bool hasSerialTiming_ = false;
    uint32_t queueWaitMs_ = 0U;
    uint32_t rtuResponseMs_ = 0U;
    uint16_t transactionId_ = 0U;
    uint16_t rtuTransactionId_ = MODBUS_TRANSACTION_INVALID;
    uint8_t unitId_ = 0U;
    uint8_t function_ = 0U;
    uint16_t requestedAddress_ = 0U;
    uint16_t requestedCount_ = 0U;
    uint16_t requestedValue_ = 0U;
    uint32_t startedAtMs_ = 0U;
    uint32_t timeoutMs_ = 0U;
    uint8_t output_[ModbusTcpAdu::kMaxAdu]{};
    size_t outputSize_ = 0U;
};
} // namespace ModbusTcpServer

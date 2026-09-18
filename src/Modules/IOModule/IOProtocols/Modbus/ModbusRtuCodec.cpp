/**
 * @file ModbusRtuCodec.cpp
 * @brief Modbus RTU codec implementation.
 */

#include "ModbusRtuCodec.h"

RegisterOperation ModbusRtuCodec::operation(const ModbusRequest& r)
{
    if (r.protocol == RegisterWireProtocol::VendorRegisterRtu) return r.operation;
    if (r.function == MODBUS_FC_WRITE_SINGLE_REGISTER) return RegisterOperation::WriteSingle;
    if (r.function == MODBUS_FC_WRITE_MULTIPLE_REGISTERS) return RegisterOperation::WriteMultiple;
    return RegisterOperation::Read;
}

bool ModbusRtuCodec::validRequest(const ModbusRequest& r)
{
    if (r.slaveAddress == 0 || r.slaveAddress > 247 || r.registerCount == 0 ||
        r.registerCount > MODBUS_MAX_REGISTERS_PER_REQUEST ||
        uint32_t(r.registerAddress) + r.registerCount > 65536U) return false;
    if (r.protocol == RegisterWireProtocol::ModbusRtu) {
        if (r.function != 0x03 && r.function != 0x04 && r.function != 0x06 && r.function != 0x10) return false;
    } else if (r.protocol == RegisterWireProtocol::VendorRegisterRtu) {
        if (!r.function || uint8_t(r.operation) > uint8_t(RegisterOperation::WriteMultiple)) return false;
    } else return false;
    return operation(r) != RegisterOperation::WriteSingle || r.registerCount == 1;
}

uint16_t ModbusRtuCodec::crc16(const uint8_t* data, size_t length)
{
    if (!data && length != 0U) return 0U;
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const bool lsb = (crc & 0x0001U) != 0U;
            crc >>= 1U;
            if (lsb) crc ^= 0xA001U;
        }
    }
    return crc;
}

void ModbusRtuCodec::appendCrc_(uint8_t* frame, size_t payloadLength)
{
    const uint16_t crc = crc16(frame, payloadLength);
    frame[payloadLength] = (uint8_t)(crc & 0xFFU);
    frame[payloadLength + 1U] = (uint8_t)(crc >> 8U);
}

uint16_t ModbusRtuCodec::readU16_(const uint8_t* data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

bool ModbusRtuCodec::encodeRequest(const ModbusRequest& request,
                                   uint8_t* outFrame,
                                   size_t capacity,
                                   size_t& outLength)
{
    outLength = 0U;
    if (!outFrame || !validRequest(request)) return false;

    switch (operation(request)) {
        case RegisterOperation::Read:
            if (capacity < 8U) return false;
            outFrame[0] = request.slaveAddress;
            outFrame[1] = request.function;
            outFrame[2] = (uint8_t)(request.registerAddress >> 8U);
            outFrame[3] = (uint8_t)(request.registerAddress & 0xFFU);
            outFrame[4] = (uint8_t)(request.registerCount >> 8U);
            outFrame[5] = (uint8_t)(request.registerCount & 0xFFU);
            appendCrc_(outFrame, 6U);
            outLength = 8U;
            return true;

        case RegisterOperation::WriteSingle:
            if (capacity < 8U || request.registerCount != 1U) return false;
            outFrame[0] = request.slaveAddress;
            outFrame[1] = request.function;
            outFrame[2] = (uint8_t)(request.registerAddress >> 8U);
            outFrame[3] = (uint8_t)(request.registerAddress & 0xFFU);
            outFrame[4] = (uint8_t)(request.values[0] >> 8U);
            outFrame[5] = (uint8_t)(request.values[0] & 0xFFU);
            appendCrc_(outFrame, 6U);
            outLength = 8U;
            return true;

        case RegisterOperation::WriteMultiple: {
            const size_t payloadLength = 7U + ((size_t)request.registerCount * 2U);
            if (capacity < payloadLength + 2U) return false;
            outFrame[0] = request.slaveAddress;
            outFrame[1] = request.function;
            outFrame[2] = (uint8_t)(request.registerAddress >> 8U);
            outFrame[3] = (uint8_t)(request.registerAddress & 0xFFU);
            outFrame[4] = (uint8_t)(request.registerCount >> 8U);
            outFrame[5] = (uint8_t)(request.registerCount & 0xFFU);
            outFrame[6] = (uint8_t)(request.registerCount * 2U);
            for (uint16_t i = 0U; i < request.registerCount; ++i) {
                outFrame[7U + i * 2U] = (uint8_t)(request.values[i] >> 8U);
                outFrame[8U + i * 2U] = (uint8_t)(request.values[i] & 0xFFU);
            }
            appendCrc_(outFrame, payloadLength);
            outLength = payloadLength + 2U;
            return true;
        }

        default:
            return false;
    }
}

ModbusResultCode ModbusRtuCodec::decodeResponse(const ModbusRequest& request,
                                                const uint8_t* frame,
                                                size_t length,
                                                ModbusResponse& outResponse)
{
    outResponse = ModbusResponse{};
    outResponse.slaveAddress = request.slaveAddress;
    outResponse.function = request.function;
    outResponse.registerAddress = request.registerAddress;
    if (!validRequest(request) || !frame || length < 5U) return MODBUS_RESULT_PROTOCOL_ERROR;

    const uint16_t expectedCrc = crc16(frame, length - 2U);
    const uint16_t receivedCrc = (uint16_t)(frame[length - 2U] |
                                            ((uint16_t)frame[length - 1U] << 8U));
    if (expectedCrc != receivedCrc) return MODBUS_RESULT_CRC_ERROR;
    if (frame[0] != request.slaveAddress) return MODBUS_RESULT_PROTOCOL_ERROR;

    if (request.protocol == RegisterWireProtocol::ModbusRtu &&
        frame[1] == (uint8_t)(request.function | 0x80U)) {
        if (length != 5U) return MODBUS_RESULT_PROTOCOL_ERROR;
        outResponse.exceptionCode = frame[2];
        return MODBUS_RESULT_EXCEPTION;
    }
    if (frame[1] != request.function) return MODBUS_RESULT_PROTOCOL_ERROR;

    if (operation(request) == RegisterOperation::Read) {
        const uint8_t byteCount = frame[2];
        if (byteCount != request.registerCount * 2U || length != (size_t)byteCount + 5U) {
            return MODBUS_RESULT_PROTOCOL_ERROR;
        }
        outResponse.registerCount = request.registerCount;
        for (uint16_t i = 0U; i < request.registerCount; ++i) {
            outResponse.values[i] = readU16_(&frame[3U + i * 2U]);
        }
        return MODBUS_RESULT_OK;
    }

    if (length != 8U || readU16_(&frame[2]) != request.registerAddress) {
        return MODBUS_RESULT_PROTOCOL_ERROR;
    }
    if (operation(request) == RegisterOperation::WriteSingle) {
        if (readU16_(&frame[4]) != request.values[0]) return MODBUS_RESULT_PROTOCOL_ERROR;
        outResponse.registerCount = 1U;
        outResponse.values[0] = request.values[0];
        return MODBUS_RESULT_OK;
    }
    if (operation(request) == RegisterOperation::WriteMultiple) {
        if (readU16_(&frame[4]) != request.registerCount) return MODBUS_RESULT_PROTOCOL_ERROR;
        outResponse.registerCount = request.registerCount;
        return MODBUS_RESULT_OK;
    }
    return MODBUS_RESULT_PROTOCOL_ERROR;
}

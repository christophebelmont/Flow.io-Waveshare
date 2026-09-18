#pragma once
/**
 * @file ModbusRtuCodec.h
 * @brief Allocation-free Modbus RTU application data unit codec.
 */

#include <stddef.h>
#include <stdint.h>

#include "Core/Services/IModbusMaster.h"

class ModbusRtuCodec {
public:
    static bool validRequest(const ModbusRequest& request);
    static RegisterOperation operation(const ModbusRequest& request);
    static uint16_t crc16(const uint8_t* data, size_t length);
    static bool encodeRequest(const ModbusRequest& request,
                              uint8_t* outFrame,
                              size_t capacity,
                              size_t& outLength);
    static ModbusResultCode decodeResponse(const ModbusRequest& request,
                                           const uint8_t* frame,
                                           size_t length,
                                           ModbusResponse& outResponse);

private:
    static void appendCrc_(uint8_t* frame, size_t payloadLength);
    static uint16_t readU16_(const uint8_t* data);
};

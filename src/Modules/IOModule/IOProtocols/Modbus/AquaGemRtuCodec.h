#pragma once
#include "ModbusRtuCodec.h"

/** AquaGem RS485 Protocol, tables 8–11. Fixed payloads, CRC high byte first. */
class AquaGemRtuCodec {
public:
    static bool validRequest(const ModbusRequest& request);
    static bool encodeRequest(const ModbusRequest&, uint8_t*, size_t, size_t&);
    static ModbusResultCode decodeResponse(const ModbusRequest&, const uint8_t*, size_t, ModbusResponse&);
};

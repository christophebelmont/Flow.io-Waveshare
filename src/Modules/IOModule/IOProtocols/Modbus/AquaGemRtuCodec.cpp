#include "AquaGemRtuCodec.h"

namespace {
uint16_t readWord(const uint8_t* bytes) { return uint16_t(bytes[0]) << 8 | bytes[1]; }
void writeWord(uint8_t* bytes, uint16_t value) { bytes[0] = value >> 8; bytes[1] = value & 0xFF; }
}

bool AquaGemRtuCodec::validRequest(const ModbusRequest& r)
{
    if (r.protocol != RegisterWireProtocol::AquaGem || r.slaveAddress != 0xAA ||
        r.registerCount != 1 || r.responseLayout != RegisterResponseLayout::Standard) return false;
    if (r.operation == RegisterOperation::Read) return r.function == 0xC3 && r.registerAddress == 2001;
    return r.operation == RegisterOperation::WriteSingle && r.function == 0xD0 && r.registerAddress == 3001 &&
        (r.values[0] == 1 || (r.values[0] >= 1200 && r.values[0] <= 2900));
}

bool AquaGemRtuCodec::encodeRequest(const ModbusRequest& r, uint8_t* frame, size_t capacity, size_t& length)
{
    length = 0;
    if (!frame || capacity < 8 || !validRequest(r)) return false;
    frame[0] = r.slaveAddress; frame[1] = r.function;
    writeWord(frame + 2, r.registerAddress);
    // Table 8 leaves these two bytes reserved. Send zero; do not interpret them as a register count.
    writeWord(frame + 4, r.operation == RegisterOperation::Read ? 0 : r.values[0]);
    const uint16_t crc = ModbusRtuCodec::crc16(frame, 6);
    frame[6] = uint8_t(crc);
    frame[7] = uint8_t(crc >> 8);
    length = 8;
    return true;
}

ModbusResultCode AquaGemRtuCodec::decodeResponse(const ModbusRequest& r, const uint8_t* frame,
                                                size_t length, ModbusResponse& out)
{
    out = {};
    if (!validRequest(r) || !frame || length != 9) return MODBUS_RESULT_PROTOCOL_ERROR;
    const uint16_t receivedCrc = uint16_t(frame[7]) | (uint16_t(frame[8]) << 8);
    if (receivedCrc != ModbusRtuCodec::crc16(frame, 7)) return MODBUS_RESULT_CRC_ERROR;
    if (frame[0] != r.slaveAddress || frame[1] != r.function) return MODBUS_RESULT_PROTOCOL_ERROR;
    out.slaveAddress = r.slaveAddress; out.function = r.function; out.registerAddress = r.registerAddress;
    if (r.operation == RegisterOperation::Read) {
        out.aquaGem = {true, readWord(frame + 2), frame[4], readWord(frame + 5)};
        out.registerCount = 1; out.values[0] = out.aquaGem.rpm;
    } else {
        if (readWord(frame + 2) != r.registerAddress) return MODBUS_RESULT_PROTOCOL_ERROR;
        // Table 11 defines bytes 4–6 as reserved, not an echo of the requested RPM.
        // Acknowledgement never substitutes for the subsequent physical status read.
    }
    return MODBUS_RESULT_OK;
}

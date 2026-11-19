#pragma once
/**
 * @file ModbusTcpAdu.h
 * @brief Allocation-free Modbus TCP ADU parsing and response encoding.
 *
 * Protocol-only helper: UART and scheduler access belong to the gateway module.
 * An MBAP length includes Unit ID + PDU, never the 6-byte MBAP prefix.
 */
#include <stddef.h>
#include <stdint.h>
#include "Core/Services/IModbusMaster.h"

namespace ModbusTcpAdu {
constexpr size_t kMaxAdu = 260U;
constexpr size_t kMbapPrefix = 6U;
constexpr uint8_t kExceptionMask = 0x80U;

struct Request {
    uint16_t transactionId = 0U;
    uint8_t unitId = 0U;
    uint8_t function = 0U;
    uint16_t address = 0U;
    uint16_t count = 0U;
    uint16_t value = 0U;
    const uint8_t* pdu = nullptr;
    uint16_t pduLength = 0U;
};

inline uint16_t word(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) << 8U | p[1]);
}
inline void putWord(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8U);
    p[1] = uint8_t(v);
}
/** Returns zero if MBAP is invalid; otherwise complete ADU size.
 * A return greater than available means more TCP bytes are required.
 */
inline size_t expectedSize(const uint8_t* bytes, size_t available) {
    if (!bytes || available < kMbapPrefix) return 0U;
    if (word(bytes + 2U) != 0U) return 0U;
    const uint16_t length = word(bytes + 4U);
    if (length < 2U || length > 254U) return 0U;
    return kMbapPrefix + length;
}
inline bool decode(const uint8_t* bytes, size_t length, Request& out) {
    const size_t expected = expectedSize(bytes, length);
    if (!expected || expected != length || length < 8U) return false;
    Request parsed{};
    parsed.transactionId = word(bytes);
    parsed.unitId = bytes[6];
    parsed.function = bytes[7];
    parsed.pdu = bytes + 7U;
    parsed.pduLength = uint16_t(length - 7U);
    // Only operations supported by the shared RS485 scheduler are accepted.
    if (parsed.function == 0x03U || parsed.function == 0x04U) {
        if (parsed.pduLength != 5U) return false;
        parsed.address = word(bytes + 8U);
        parsed.count = word(bytes + 10U);
        if (!parsed.count || parsed.count > MODBUS_MAX_READ_REGISTERS) return false;
    } else if (parsed.function == 0x06U) {
        if (parsed.pduLength != 5U) return false;
        parsed.address = word(bytes + 8U);
        parsed.value = word(bytes + 10U);
        parsed.count = 1U;
    } else if (parsed.function == 0x10U) {
        if (parsed.pduLength < 6U) return false;
        parsed.address = word(bytes + 8U);
        parsed.count = word(bytes + 10U);
        const uint8_t byteCount = bytes[12];
        if (!parsed.count || parsed.count > MODBUS_MAX_WRITE_REGISTERS ||
            byteCount != parsed.count * 2U ||
            parsed.pduLength != size_t(6U + byteCount)) return false;
    } else return false;
    if (uint32_t(parsed.address) + parsed.count > 65536U) return false;
    out = parsed;
    return true;
}
inline size_t response(uint16_t transactionId, uint8_t unitId,
                       const uint8_t* pdu, size_t pduLength,
                       uint8_t* out, size_t capacity) {
    if (!pdu || !out || pduLength == 0U || pduLength > 253U ||
        capacity < 7U + pduLength) return 0U;
    putWord(out, transactionId);
    putWord(out + 2U, 0U);
    putWord(out + 4U, uint16_t(1U + pduLength));
    out[6] = unitId;
    for (size_t i = 0; i < pduLength; ++i) out[7U + i] = pdu[i];
    return 7U + pduLength;
}
inline size_t exception(uint16_t transactionId, uint8_t unitId,
                        uint8_t function, uint8_t code,
                        uint8_t* out, size_t capacity) {
    const uint8_t pdu[2] = {uint8_t(function | kExceptionMask), code};
    return response(transactionId, unitId, pdu, sizeof(pdu), out, capacity);
}
} // namespace ModbusTcpAdu

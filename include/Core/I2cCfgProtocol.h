#pragma once
/**
 * @file I2cCfgProtocol.h
 * @brief Shared I2C protocol constants for Flow.IO cfg remote access.
 */

#include <stddef.h>
#include <stdint.h>

namespace I2cCfgProtocol {

constexpr uint8_t ReqMagic = 0xA5;
constexpr uint8_t RespMagic = 0x5A;
constexpr uint8_t Version = 1;
constexpr uint8_t CapabilityVersionFrameCrc = 2;

constexpr size_t MaxPayload = 96;
constexpr size_t FrameCrcSize = 2;
constexpr size_t ReqHeaderSize = 5;   // magic, ver, op, seq, payload_len
constexpr size_t RespHeaderSize = 6;  // magic, ver, op, seq, status, payload_len
constexpr size_t MaxReqFrame = ReqHeaderSize + MaxPayload + FrameCrcSize;
constexpr size_t MaxRespFrame = RespHeaderSize + MaxPayload + FrameCrcSize;

enum Op : uint8_t {
    OpPing = 0x01,
    OpListCount = 0x10,
    OpListItem = 0x11,
    OpListChildrenCount = 0x12,
    OpListChildrenItem = 0x13,
    OpGetModuleBegin = 0x20,
    OpGetModuleChunk = 0x21,
    OpGetRuntimeStatusBegin = 0x22,
    OpGetRuntimeStatusChunk = 0x23,
    OpGetRuntimeAlarmBegin = 0x24,
    OpGetRuntimeAlarmChunk = 0x25,
    OpGetRuntimeUiValues = 0x26,
    OpPatchBegin = 0x30,
    OpPatchWrite = 0x31,
    OpPatchCommit = 0x32,
    OpSystemAction = 0x40
};

enum StatusDomain : uint8_t {
    StatusDomainSystem = 1,
    StatusDomainWifi = 2,
    StatusDomainMqtt = 3,
    StatusDomainI2c = 4,
    StatusDomainPool = 5,
    StatusDomainAlarm = 6
};

enum Status : uint8_t {
    StatusOk = 0,
    StatusBadRequest = 1,
    StatusNotReady = 2,
    StatusRange = 3,
    StatusOverflow = 4,
    StatusFailed = 5,
    StatusFrameCrc = 6,
    StatusFrameFormat = 7
};

static inline uint16_t crc16Ccitt(const uint8_t* data, size_t len, uint16_t seed = 0xFFFFu)
{
    uint16_t crc = seed;
    if (!data || len == 0U) return crc;
    for (size_t i = 0U; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static inline uint16_t readFrameCrcLe(const uint8_t* data, size_t len)
{
    if (!data || len < FrameCrcSize) return 0u;
    return (uint16_t)((uint16_t)data[len - FrameCrcSize] |
                      ((uint16_t)data[len - FrameCrcSize + 1U] << 8));
}

}  // namespace I2cCfgProtocol

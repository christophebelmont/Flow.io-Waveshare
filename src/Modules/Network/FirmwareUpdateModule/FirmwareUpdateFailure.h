#pragma once
/**
 * @file FirmwareUpdateFailure.h
 * @brief Versioned, bounded record of the last OTA failure. No dynamic memory.
 *
 * Stored only after an OTA error; successful updates intentionally do NOT
 * erase it. The numeric code is a broad category, not an esp_err_t.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

namespace OtaFailure {
constexpr uint32_t kMagic = 0x4F544146U; // "OTAF"
constexpr uint16_t kVersion = 1U;

// Generic categories, not platform-specific ESP-IDF error codes.
enum Code : uint8_t {
    Other = 1,
    NetworkHttp = 2,
    Manifest = 3,
    IntegritySha = 4,
    FlashPartition = 5,
    CommitReboot = 6,
    Timeout = 7
};

struct Record {
    uint32_t magic = 0U;
    uint16_t version = 0U;
    uint8_t code = 0U;
    uint8_t target = 0U;
    uint32_t operationId = 0U;
    char stage[28]{};
    char reason[96]{};
    uint32_t checksum = 0U;
};
static_assert(sizeof(Record) <= 160U, "OTA failure NVS record must remain small");

inline uint32_t checksum(const Record& record)
{
    uint32_t hash = 2166136261U;
    auto mix = [&hash](uint8_t value) {
        hash = (hash ^ value) * 16777619U;
    };
    const uint32_t fields[] = {
        record.magic, (uint32_t)record.version, (uint32_t)record.code,
        (uint32_t)record.target, record.operationId
    };
    for (uint32_t value : fields)
        for (uint8_t byte = 0U; byte < 4U; ++byte)
            mix((uint8_t)(value >> (byte * 8U)));
    for (char value : record.stage) mix((uint8_t)value);
    for (char value : record.reason) mix((uint8_t)value);
    return hash;
}

inline bool valid(const Record& record)
{
    return record.magic == kMagic &&
           record.version == kVersion &&
           record.operationId != 0U &&
           record.code >= Other && record.code <= Timeout &&
           memchr(record.stage, 0, sizeof(record.stage)) != nullptr &&
           memchr(record.reason, 0, sizeof(record.reason)) != nullptr &&
           record.checksum == checksum(record);
}

inline uint8_t category(const char* message)
{
    if (!message) return Other;
    if (strstr(message, "timeout") || strstr(message, "timed out") ||
        strstr(message, "stalled")) return Timeout;
    if (strstr(message, "SHA") || strstr(message, "sha") ||
        strstr(message, "digest") || strstr(message, "checksum"))
        return IntegritySha;
    if (strstr(message, "HTTP") || strstr(message, "http") ||
        strstr(message, "network") || strstr(message, "download") ||
        strstr(message, "Range")) return NetworkHttp;
    if (strstr(message, "manifest") || strstr(message, "version"))
        return Manifest;
    if (strstr(message, "flash") || strstr(message, "partition") ||
        strstr(message, "erase") || strstr(message, "write"))
        return FlashPartition;
    if (strstr(message, "commit") || strstr(message, "boot") ||
        strstr(message, "counter checkpoint") || strstr(message, "persist"))
        return CommitReboot;
    return Other;
}

inline Record make(uint8_t target, uint32_t operationId,
                   const char* stage, const char* reason)
{
    Record r{};
    r.magic = kMagic;
    r.version = kVersion;
    r.code = category(reason);
    r.target = target;
    r.operationId = operationId;
    snprintf(r.stage, sizeof(r.stage), "%s", stage ? stage : "unknown");
    snprintf(r.reason, sizeof(r.reason), "%s", reason ? reason : "failed");
    r.checksum = checksum(r);
    return r;
}
} // namespace OtaFailure

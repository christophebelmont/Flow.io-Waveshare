/**
 * @file RebootDiagnostics.cpp
 * @brief Software-reset breadcrumbs, without writing to flash/NVS.
 */
#include "Core/RebootDiagnostics.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace RebootDiagnostics {
namespace {

constexpr uint32_t kMagic = 0x52424F54U; // "RBOT"
constexpr uint32_t kVersion = 1U;
constexpr uint32_t kChecksumSeed = 0xA9F32E71U;

struct Record {
    uint32_t magic;
    uint32_t version;
    uint32_t plannedOrigin;
    uint32_t plannedOperationId;
    uint32_t lastOtaOperationId;
    uint32_t lastOtaStage;
    uint32_t receivedBytes;
    uint32_t totalBytes;
    uint32_t checksum;
};

// Deliberately NOT initialized. Validated on every boot using magic/version
// and checksum. RTC_NOINIT survives software restart, but not power removal.
RTC_NOINIT_ATTR volatile Record rtcRecord;

uint32_t checksumOf_(const Record& record)
{
    return kChecksumSeed ^ record.version ^ record.plannedOrigin ^
           record.plannedOperationId ^ record.lastOtaOperationId ^
           record.lastOtaStage ^ record.receivedBytes ^ record.totalBytes;
}

Record readRecord_()
{
    Record record{};
    record.magic = rtcRecord.magic;
    record.version = rtcRecord.version;
    record.plannedOrigin = rtcRecord.plannedOrigin;
    record.plannedOperationId = rtcRecord.plannedOperationId;
    record.lastOtaOperationId = rtcRecord.lastOtaOperationId;
    record.lastOtaStage = rtcRecord.lastOtaStage;
    record.receivedBytes = rtcRecord.receivedBytes;
    record.totalBytes = rtcRecord.totalBytes;
    record.checksum = rtcRecord.checksum;
    if (record.magic != kMagic || record.version != kVersion ||
        record.checksum != checksumOf_(record)) {
        return Record{};
    }
    return record;
}

void writeRecord_(const Record& record)
{
    // Invalid until the very last store; an interrupted write is not
    // reported as an intentional reboot on the next boot.
    rtcRecord.magic = 0U;
    rtcRecord.version = kVersion;
    rtcRecord.plannedOrigin = record.plannedOrigin;
    rtcRecord.plannedOperationId = record.plannedOperationId;
    rtcRecord.lastOtaOperationId = record.lastOtaOperationId;
    rtcRecord.lastOtaStage = record.lastOtaStage;
    rtcRecord.receivedBytes = record.receivedBytes;
    rtcRecord.totalBytes = record.totalBytes;
    rtcRecord.checksum = checksumOf_(record);
    rtcRecord.magic = kMagic;
}

const char* originName_(uint32_t origin)
{
    switch (static_cast<Origin>(origin)) {
        case Origin::OtaRelease: return "ota_release";
        case Origin::OtaNextion: return "ota_nextion";
        case Origin::System: return "system";
        case Origin::FactoryReset: return "factory_reset";
        default: return "none";
    }
}

const char* stageName_(uint32_t stage)
{
    switch (static_cast<OtaStage>(stage)) {
        case OtaStage::Manifest: return "manifest";
        case OtaStage::Spiffs: return "spiffs_write";
        case OtaStage::Firmware: return "firmware_write";
        case OtaStage::VerifySpiffs: return "spiffs_verify";
        case OtaStage::VerifyFirmware: return "firmware_verify";
        case OtaStage::Commit: return "commit";
        default: return "none";
    }
}

} // namespace

void noteOta(uint32_t operationId, OtaStage stage,
             uint32_t receivedBytes, uint32_t totalBytes)
{
    Record record = readRecord_();
    record.lastOtaOperationId = operationId;
    record.lastOtaStage = static_cast<uint32_t>(stage);
    record.receivedBytes = receivedBytes;
    record.totalBytes = totalBytes;
    writeRecord_(record);
}

void markPlanned(Origin origin, uint32_t operationId)
{
    Record record = readRecord_();
    record.plannedOrigin = static_cast<uint32_t>(origin);
    record.plannedOperationId = operationId;
    writeRecord_(record);
}

void reportPreviousBoot()
{
    const Record record = readRecord_();
    rtcRecord.magic = 0U; // One-shot: avoid stale intent on a subsequent reset.
    Serial.printf("[BOOT-RESET] reset_reason=%u rtc_valid=%u planned=%s planned_op=%lu last_ota_op=%lu stage=%s received=%lu/%lu\n",
                  (unsigned)esp_reset_reason(),
                  record.magic == kMagic ? 1U : 0U,
                  originName_(record.plannedOrigin),
                  (unsigned long)record.plannedOperationId,
                  (unsigned long)record.lastOtaOperationId,
                  stageName_(record.lastOtaStage),
                  (unsigned long)record.receivedBytes,
                  (unsigned long)record.totalBytes);
}

} // namespace RebootDiagnostics

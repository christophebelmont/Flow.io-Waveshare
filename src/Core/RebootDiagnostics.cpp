/**
 * @file RebootDiagnostics.cpp
 * @brief Software-reset breadcrumbs, without writing to flash/NVS.
 */
#include "Core/RebootDiagnostics.h"
#include "Core/Log.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <nvs.h>

namespace RebootDiagnostics {
namespace {

constexpr uint32_t kMagic = 0x52424F54U; // "RBOT"
constexpr uint32_t kVersion = 1U;
constexpr uint32_t kChecksumSeed = 0xA9F32E71U;
// Dedicated namespace: never write the shared configuration store at OTA hot-path checkpoints.
constexpr char kBootNvsNamespace[] = "flowio_boot";
constexpr char kBootNvsKey[] = "planned";

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

bool isValidRecord_(const Record& record)
{
    return record.magic == kMagic && record.version == kVersion &&
           record.checksum == checksumOf_(record);
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
    if (!isValidRecord_(record)) {
        return Record{};
    }
    return record;
}

void writeRecord_(const Record& record)
{
    // The first noteOta() after boot starts with an empty record (version=0).
    // Calculate the checksum from the EXACT normalized fields written to RTC.
    // Previously RTC.version was kVersion but checksumOf_(record) used version=0;
    // every noteOta() then failed validation, dropping the last OTA stage.
    Record stored = record;
    stored.magic = kMagic;
    stored.version = kVersion;
    stored.checksum = checksumOf_(stored);
    // Invalid until the very last store; an interrupted write is not
    // reported as an intentional reboot on the next boot.
    rtcRecord.magic = 0U;
    rtcRecord.version = stored.version;
    rtcRecord.plannedOrigin = stored.plannedOrigin;
    rtcRecord.plannedOperationId = stored.plannedOperationId;
    rtcRecord.lastOtaOperationId = stored.lastOtaOperationId;
    rtcRecord.lastOtaStage = stored.lastOtaStage;
    rtcRecord.receivedBytes = stored.receivedBytes;
    rtcRecord.totalBytes = stored.totalBytes;
    rtcRecord.checksum = stored.checksum;
    rtcRecord.magic = stored.magic;
}

// Only markPlanned() writes this backup, immediately before a deliberate restart.
bool savePlannedNvs_(const Record& record)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kBootNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    err = nvs_set_blob(handle, kBootNvsKey, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

// One-shot read on boot. Always remove the marker after reading, even if
// validation fails, to avoid incorrectly attributing a later reset to OTA.
bool takePlannedNvs_(Record& record)
{
    nvs_handle_t handle = 0;
    const esp_err_t openErr = nvs_open(kBootNvsNamespace, NVS_READWRITE, &handle);
    if (openErr != ESP_OK) return false;
    size_t length = sizeof(record);
    const esp_err_t readErr = nvs_get_blob(handle, kBootNvsKey, &record, &length);
    const bool present = readErr == ESP_OK || readErr == ESP_ERR_NVS_INVALID_LENGTH;
    const bool valid = readErr == ESP_OK && length == sizeof(record) && isValidRecord_(record);
    if (present) {
        esp_err_t clearErr = nvs_erase_key(handle, kBootNvsKey);
        if (clearErr == ESP_OK) clearErr = nvs_commit(handle);
        if (clearErr != ESP_OK)
            Serial.printf("[BOOT-RESET] nvs_clear_failed err=0x%x\n", (unsigned)clearErr);
    }
    nvs_close(handle);
    return valid;
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
        case OtaStage::SpiffsRamDownload: return "spiffs_ram_download";
        case OtaStage::FirmwareRamDownload: return "firmware_ram_download";
        case OtaStage::SpiffsRamVerify: return "spiffs_ram_verify";
        case OtaStage::FirmwareRamVerify: return "firmware_ram_verify";
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
    record.magic = kMagic;
    record.version = kVersion;
    record.checksum = checksumOf_(record);
    writeRecord_(record);
    const bool persisted = savePlannedNvs_(record);
    Serial.printf("[BOOT-RESET] planned_mark source=rtc+nvs nvs_saved=%u op=%lu\n",
                  persisted ? 1U : 0U, (unsigned long)operationId);
}

void reportPreviousBoot()
{
    const esp_reset_reason_t reset = esp_reset_reason();
    const Record rtc = readRecord_();
    const bool rtcValid = isValidRecord_(rtc);
    Record persisted{};
    const bool nvsValid = takePlannedNvs_(persisted);
    // A stale persistent marker after power loss or panic is NOT evidence
    // that this startup came from our deliberate OTA restart.
    const bool useNvs = !rtcValid && nvsValid && reset == ESP_RST_SW;
    const Record record = rtcValid ? rtc : (useNvs ? persisted : Record{});
    rtcRecord.magic = 0U; // One-shot: avoid stale intent on a subsequent reset.
    Serial.printf("[BOOT-RESET] reset_reason=%u rtc_valid=%u nvs_valid=%u source=%s planned=%s planned_op=%lu last_ota_op=%lu stage=%s received=%lu/%lu\n",
                  (unsigned)reset,
                  rtcValid ? 1U : 0U,
                  nvsValid ? 1U : 0U,
                  rtcValid ? "rtc" : (useNvs ? "nvs" : "none"),
                  originName_(record.plannedOrigin),
                  (unsigned long)record.plannedOperationId,
                  (unsigned long)record.lastOtaOperationId,
                  stageName_(record.lastOtaStage),
                  (unsigned long)record.receivedBytes,
                  (unsigned long)record.totalBytes);

    // The detailed Serial line is deliberately retained for early boot and
    // USB capture. The Web bootlog uses the bounded LogHub entry instead.
    // Publish short entries so the reset source and last OTA stage survive
    // the 159-character LogEntry payload limit.
    if (Log::hub()) {
        const LogModuleId module = (LogModuleId)LogModuleIdValue::FirmwareUpdateModule;
        Log::info(module,
                  "[BOOT-RESET] reason=%u rtc_valid=%u nvs_valid=%u source=%s planned=%s",
                  (unsigned)reset, rtcValid ? 1U : 0U, nvsValid ? 1U : 0U,
                  rtcValid ? "rtc" : (useNvs ? "nvs" : "none"),
                  originName_(record.plannedOrigin));
        Log::info(module,
                  "[BOOT-RESET] planned_op=%lu last_ota_op=%lu stage=%s received=%lu/%lu",
                  (unsigned long)record.plannedOperationId,
                  (unsigned long)record.lastOtaOperationId,
                  stageName_(record.lastOtaStage),
                  (unsigned long)record.receivedBytes,
                  (unsigned long)record.totalBytes);
    }
}

} // namespace RebootDiagnostics

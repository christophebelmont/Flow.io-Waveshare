/**
 * @file FirmwareUpdateModule.cpp
 * @brief Firmware updater implementation.
 */

#include "FirmwareUpdateModule.h"
#include "Core/PerformanceDiagnostics.h"
#include "Core/CounterPersistence.h"
#include "Core/RebootDiagnostics.h"
#include "Core/OtaPsramPolicy.h"
#include "Core/OtaHttpRetryPolicy.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <FS.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ctype.h>
#include <string.h>
#include <new>
#include <memory>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "App/BuildFlags.h"
#include "Board/BoardSpec.h"
#include "Core/BoundedBufferStream.h"
#include "Core/SpiRamJsonDocument.h"
#include "Core/ErrorCodes.h"
#include "Core/FirmwareVersion.h"
#include "Core/NvsKeys.h"
#include "Core/SystemLimits.h"
#include "Modules/HMIModule/Drivers/NextionDisplayIdentity.h"

#include <ESPNexUpload.h>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::FirmwareUpdateModule)
#include "Core/ModuleLog.h"

static bool writeSimpleError_(char* out, size_t outLen, const char* msg);

namespace {

class SemaphoreGuard {
public:
    explicit SemaphoreGuard(SemaphoreHandle_t semaphore,
                            TickType_t waitTicks = portMAX_DELAY)
        : semaphore_(semaphore),
          acquired_(semaphore_ && xSemaphoreTake(semaphore_, waitTicks) == pdTRUE)
    {
    }

    ~SemaphoreGuard()
    {
        if (acquired_) xSemaphoreGive(semaphore_);
    }

    SemaphoreGuard(const SemaphoreGuard&) = delete;
    SemaphoreGuard& operator=(const SemaphoreGuard&) = delete;

    bool acquired() const { return acquired_; }

private:
    SemaphoreHandle_t semaphore_ = nullptr;
    bool acquired_ = false;
};

class PartitionWriteStream final : public Stream {
public:
    using ProgressCallback = void (*)(void* context, size_t writtenBytes);

    PartitionWriteStream(const esp_partition_t* partition,
                         ProgressCallback progressCallback,
                         void* progressContext)
        : partition_(partition),
          progressCallback_(progressCallback),
          progressContext_(progressContext)
    {
    }

    size_t write(uint8_t value) override { return write(&value, 1U); }

    size_t write(const uint8_t* data, size_t size) override
    {
        if (!partition_ || !data || size == 0U) return 0U;
        if (offset_ > partition_->size || size > partition_->size - offset_) {
            overflow_ = true;
            setWriteError();
            return 0U;
        }
        if (esp_partition_write(partition_, offset_, data, size) != ESP_OK) {
            flashError_ = true;
            setWriteError();
            return 0U;
        }
        offset_ += size;
        if (progressCallback_) progressCallback_(progressContext_, size);
        return size;
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    size_t writtenBytes() const { return offset_; }
    bool overflowed() const { return overflow_; }
    bool flashError() const { return flashError_; }

private:
    const esp_partition_t* partition_ = nullptr;
    ProgressCallback progressCallback_ = nullptr;
    void* progressContext_ = nullptr;
    size_t offset_ = 0U;
    bool overflow_ = false;
    bool flashError_ = false;
};

const LocalUiBoardSpec& localUiBoardSpec_(const BoardSpec& board)
{
    // Safety fallback used only when the selected BoardSpec does not expose
    // a local UI extension block (board.localUi == nullptr).
    static constexpr LocalUiBoardSpec kFallback{
        {
            240,
            320,
            1,
            0,
            0,
            14,
            15,
            4,
            5,
            35,
            18,
            19,
            false,
            true,
            8000000U,
            80
        },
        {
            36,
            120,
            true,
            23,
            40,
            false,
            5000
        },
        {
            25,
            26,
            13,
            115200U
        }
    };
    const LocalUiBoardSpec* cfg = boardLocalUiConfig(board);
    return cfg ? *cfg : kFallback;
}

const UartSpec& panelUartSpec_(const BoardSpec& board)
{
    static constexpr UartSpec kFallback{"panel", 2, 33, 32, 115200, false, -1};
    const UartSpec* spec = boardFindUart(board, "panel");
    if (!spec) spec = boardFindUart(board, "hmi");
    return spec ? *spec : kFallback;
}

bool manifestCheckIsActive_(FirmwareManifestCheckState state)
{
    return state == FirmwareManifestCheckState::Queued ||
           state == FirmwareManifestCheckState::Downloading;
}

bool isSimpleArtifactFilename_(const char* path)
{
    if (!path || path[0] == '\0') return false;
    for (size_t i = 0U; path[i] != '\0'; ++i) {
        const char value = path[i];
        if (!isalnum((unsigned char)value) && value != '.' && value != '_' && value != '-') {
            return false;
        }
    }
    return strstr(path, "..") == nullptr;
}

bool buildManifestSiblingUrl_(const char* manifestUrl,
                              const char* path,
                              char* out,
                              size_t outLen)
{
    if (!manifestUrl || !path || !out || outLen == 0U || !isSimpleArtifactFilename_(path)) return false;
    const char* slash = strrchr(manifestUrl, '/');
    if (!slash) return false;
    const size_t baseLen = (size_t)(slash - manifestUrl) + 1U;
    const size_t pathLen = strlen(path);
    if (baseLen + pathLen + 1U > outLen) return false;
    memcpy(out, manifestUrl, baseLen);
    memcpy(out + baseLen, path, pathLen + 1U);
    return true;
}

bool parseSha256_(const char* text, uint8_t out[32])
{
    if (!text || !out || strlen(text) != 64U) return false;
    for (size_t i = 0U; i < 32U; ++i) {
        const char hi = text[i * 2U];
        const char lo = text[i * 2U + 1U];
        const auto nibble = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        };
        const int high = nibble(hi);
        const int low = nibble(lo);
        if (high < 0 || low < 0) return false;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

bool sha256Matches_(const uint8_t actual[32], const uint8_t expected[32])
{
    uint8_t difference = 0U;
    for (size_t i = 0U; i < 32U; ++i) difference |= actual[i] ^ expected[i];
    return difference == 0U;
}

bool isLocalReleaseImageTarget_(FirmwareUpdateTarget target)
{
    return target == FirmwareUpdateTarget::Spiffs ||
           target == FirmwareUpdateTarget::Waveshare;
}

bool candidateFilesystemIsValid_(const char* label, const char* version, const char* hardware)
{
    if (!label || label[0] == '\0') return false;
    fs::SPIFFSFS candidate;
    if (!candidate.begin(false, "/candidate", 4, label)) return false;
    // A complete paired release must contain the new UI migration marker.
    // A legacy SPIFFS image must never be accepted as the final A/B pair.
    const bool valid = ReleaseStorage::validateReleaseFilesystem(candidate, version, hardware) &&
                       candidate.exists("/ota-paired-v1.marker");
    candidate.end();
    return valid;
}

static void otaMonitor_(const char* phase)
{
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    // No allocation, filesystem I/O or NVS writes from these probes.
    // Values are bytes, including FreeRTOS's ESP32 stack high-water mark.
    const uint32_t freeInternal = heap_caps_get_free_size(internal);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(internal);
    const uint32_t fragmentation = freeInternal
        ? 100U - (uint32_t)((uint64_t)largestInternal * 100U / freeInternal) : 0U;
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    const uint32_t hwm = uxTaskGetStackHighWaterMark(task);
    // LogEntry.msg holds only 159 printable characters (LOG_MSG_MAX=160).
    // Keep the stack watermark FIRST and every line below that limit.
    // Do not increase LogEntry size: it also expands the LogHub/boot buffers.
    LOGI("[OTA-MEM] phase=%s task=%s id=%p core=%d hwm_bytes=%lu internal=%lu largest=%lu",
         phase, pcTaskGetName(task), (void*)task, (int)xPortGetCoreID(),
         (unsigned long)hwm, (unsigned long)freeInternal,
         (unsigned long)largestInternal);
    LOGI("[OTA-HEAP] phase=%s ms=%lu min_internal=%lu frag_pct=%lu psram=%lu largest_psram=%lu",
         phase, (unsigned long)millis(),
         (unsigned long)heap_caps_get_minimum_free_size(internal),
         (unsigned long)fragmentation,
         (unsigned long)heap_caps_get_free_size(external),
         (unsigned long)heap_caps_get_largest_free_block(external));
    if (hwm < 1024U) {
        LOGW("[OTA-STACK] LOW phase=%s task=%s handle=%p hwm_bytes=%lu",
             phase, pcTaskGetName(task), (void*)task, (unsigned long)hwm);
    }
}

bool cloneActiveReleaseFilesystem_(char* errOut, size_t errOutLen)
{
    const esp_partition_t* source = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        ReleaseStorage::filesystemLabel(ReleaseStorage::runningSlot()));
    const esp_partition_t* destination = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        ReleaseStorage::filesystemLabel(ReleaseStorage::inactiveSlot()));
    if (!source || !destination || source->size != destination->size) {
        return writeSimpleError_(errOut, errOutLen, "release filesystem slots unavailable");
    }
    LOGI("[OTA] phase=clone_start source=%s destination=%s bytes=%lu",
         source->label, destination->label, (unsigned long)source->size);
    otaMonitor_("clone_before");
#if FLOW_PERFORMANCE_DIAGNOSTICS
    const uint32_t cloneStartMs = millis();
    const uint32_t eraseStartMs = millis();
#endif
    if (esp_partition_erase_range(destination, 0U, destination->size) != ESP_OK) {
        LOGE("[OTA] phase=clone_erase_failed");
#if FLOW_PERFORMANCE_DIAGNOSTICS
        LOGD("[OTA] timing=clone_erase_failed elapsed_ms=%lu", (unsigned long)(millis() - eraseStartMs));
#endif
        otaMonitor_("clone_erase_failed");
        return writeSimpleError_(errOut, errOutLen, "inactive filesystem erase failed");
    }
    LOGI("[OTA] phase=clone_erase_done");
#if FLOW_PERFORMANCE_DIAGNOSTICS
    LOGD("[OTA] timing=clone_erase_done elapsed_ms=%lu", (unsigned long)(millis() - eraseStartMs));
#endif
    otaMonitor_("clone_erase_done");
    uint8_t buffer[1024]{};
#if FLOW_PERFORMANCE_DIAGNOSTICS
    const uint32_t copyStartMs = millis();
#endif
    size_t nextProgress = 256U * 1024U;
    for (size_t offset = 0U; offset < source->size; offset += sizeof(buffer)) {
        const size_t count = ((source->size - offset) < sizeof(buffer))
                                 ? (source->size - offset)
                                 : sizeof(buffer);
        if (esp_partition_read(source, offset, buffer, count) != ESP_OK ||
            esp_partition_write(destination, offset, buffer, count) != ESP_OK) {
            LOGE("[OTA] phase=clone_failed offset=%lu", (unsigned long)offset);
#if FLOW_PERFORMANCE_DIAGNOSTICS
            LOGD("[OTA] timing=clone_failed elapsed_ms=%lu", (unsigned long)(millis() - cloneStartMs));
#endif
            otaMonitor_("clone_failed");
            return writeSimpleError_(errOut, errOutLen, "inactive filesystem clone failed");
        }
        const size_t copied = offset + count;
        if (copied >= nextProgress) {
            LOGI("[OTA] phase=clone_progress copied=%lu total=%lu",
                 (unsigned long)copied, (unsigned long)source->size);
#if FLOW_PERFORMANCE_DIAGNOSTICS
            LOGD("[OTA] timing=clone_progress elapsed_ms=%lu", (unsigned long)(millis() - copyStartMs));
#endif
            otaMonitor_("clone_progress");
            nextProgress += 256U * 1024U;
        }
    }
    LOGI("[OTA] phase=clone_done");
#if FLOW_PERFORMANCE_DIAGNOSTICS
    LOGD("[OTA] timing=clone_done elapsed_ms=%lu", (unsigned long)(millis() - cloneStartMs));
#endif
    otaMonitor_("clone_done");
    return true;
}

}  // namespace

FirmwareUpdateModule::FirmwareUpdateModule(const BoardSpec& board)
{
    localReleaseMutex_ = xSemaphoreCreateMutexStatic(&localReleaseMutexStorage_);
    hardwareName_ = board.name;
    const LocalUiBoardSpec& boardCfg = localUiBoardSpec_(board);
    const UartSpec& panelUart = panelUartSpec_(board);
    flowIoEnablePin_ = boardCfg.update.flowIoEnablePin;
    nextionRxPin_ = panelUart.rxPin;
    nextionTxPin_ = panelUart.txPin;
    nextionRebootPin_ = boardCfg.update.nextionRebootPin;
    nextionUploadBaud_ = boardCfg.update.nextionUploadBaud;
}

static bool writeSimpleError_(char* out, size_t outLen, const char* msg)
{
    // This is an error-path helper. Its boolean result is propagated by
    // callers (including OTA stream failures), so writing an error message
    // successfully MUST NOT turn an unsuccessful operation into success.
    if (out && outLen > 0U) {
        (void)snprintf(out, outLen, "%s", msg ? msg : "failed");
    }
    return false;
}

static bool parseReqJsonObject_(const char* json, StaticJsonDocument<256>& doc)
{
    if (!json || json[0] == '\0') return false;
    const auto err = deserializeJson(doc, json);
    return !err && doc.is<JsonObjectConst>();
}

static void sanitizeJsonString_(char* s)
{
    if (!s) return;
    for (size_t i = 0; s[i] != '\0'; ++i) {
        if (s[i] == '"' || s[i] == '\\' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t') {
            s[i] = ' ';
        }
    }
}

static bool fileContainsToken_(fs::FS& fs, const char* path, const char* token)
{
    if (!path || !token || token[0] == '\0') return false;
    File f = fs.open(path, FILE_READ);
    if (!f) return false;

    const size_t tokLen = strlen(token);
    size_t match = 0;
    while (f.available()) {
        const int ch = f.read();
        if (ch < 0) break;
        if ((char)ch == token[match]) {
            ++match;
            if (match == tokLen) {
                f.close();
                return true;
            }
            continue;
        }
        match = ((char)ch == token[0]) ? 1U : 0U;
    }
    f.close();
    return false;
}

static bool validateCfgDocsFile_(fs::FS& fs, const char* path, char* errOut, size_t errOutLen)
{
    if (!path) {
        writeSimpleError_(errOut, errOutLen, "cfgdocs path null");
        return false;
    }
    File f = fs.open(path, FILE_READ);
    if (!f) {
        writeSimpleError_(errOut, errOutLen, "cfgdocs open failed");
        return false;
    }
    const size_t size = (size_t)f.size();
    f.close();
    if (size < 16U) {
        writeSimpleError_(errOut, errOutLen, "cfgdocs too small");
        return false;
    }
    if (!fileContainsToken_(fs, path, "\"docs\"")) {
        writeSimpleError_(errOut, errOutLen, "cfgdocs missing docs");
        return false;
    }
    return true;
}

static void configureDownloadHttp_(HTTPClient& http)
{
    http.setReuse(false);
    http.setConnectTimeout(Limits::FirmwareUpdate::Http::ConnectTimeoutMs);
    http.setTimeout(Limits::FirmwareUpdate::Http::RequestTimeoutMs);
}

static bool writeHttpBeginFailedError_(const char* resourceLabel,
                                       const char* url,
                                       char* errOut,
                                       size_t errOutLen)
{
    const char* resource = (resourceLabel && resourceLabel[0] != '\0') ? resourceLabel : "ressource";
    LOGE("HTTP begin failed resource=%s url=%s", resource, url ? url : "-");
    return writeSimpleError_(errOut, errOutLen, "serveur HTTP injoignable");
}

static bool writeHttpCodeFailedError_(const char* resourceLabel,
                                      const char* url,
                                      HTTPClient& http,
                                      int code,
                                      char* errOut,
                                      size_t errOutLen)
{
    const char* resource = (resourceLabel && resourceLabel[0] != '\0') ? resourceLabel : "fichier";
    const String raw = http.errorToString(code);
    const char* rawErr = raw.c_str();
    char msg[96] = {0};

    if (code == 404) {
        snprintf(msg, sizeof(msg), "%s introuvable (404)", resource);
    } else if (code < 0) {
        snprintf(msg, sizeof(msg), "serveur HTTP injoignable");
    } else {
        snprintf(msg, sizeof(msg), "erreur HTTP %d", code);
    }

    LOGE("HTTP request failed resource=%s code=%d err=%s url=%s", resource, code, rawErr, url ? url : "-");
    return writeSimpleError_(errOut, errOutLen, msg);
}

static bool appendUrlSegment_(char* out, size_t outLen, const char* segment)
{
    if (!out || outLen == 0) return false;
    if (!segment || segment[0] == '\0') return true;

    while (*segment == '/') ++segment;
    if (*segment == '\0') return true;

    const size_t len = strlen(out);
    if (len >= outLen) return false;
    const bool needSlash = len > 0 && out[len - 1] != '/';
    const int n = snprintf(out + len, outLen - len, "%s%s", needSlash ? "/" : "", segment);
    return n >= 0 && (size_t)n < (outLen - len);
}

const char* FirmwareUpdateModule::stateStr_(UpdateState s)
{
    switch (s) {
        case UpdateState::Idle: return "idle";
        case UpdateState::Queued: return "queued";
        case UpdateState::Downloading: return "downloading";
        case UpdateState::Flashing: return "flashing";
        case UpdateState::Rebooting: return "rebooting";
        case UpdateState::Done: return "done";
        case UpdateState::Error: return "error";
        default: return "unknown";
    }
}

const char* FirmwareUpdateModule::targetStr_(FirmwareUpdateTarget t)
{
    switch (t) {
        case FirmwareUpdateTarget::Nextion: return "nextion";
        case FirmwareUpdateTarget::Waveshare: return "waveshare";
        case FirmwareUpdateTarget::Spiffs: return "spiffs";
        default: return "unknown";
    }
}

void FirmwareUpdateModule::setStatus_(UpdateState state,
                                      FirmwareUpdateTarget target,
                                      uint8_t progress,
                                      const char* msg,
                                      uint32_t operationId)
{
    portENTER_CRITICAL(&lock_);
    status_.state = state;
    status_.operationId = operationId;
    status_.target = target;
    status_.progress = progress;
    status_.updatedAtMs = millis();
    if (!msg) msg = "";
    snprintf(status_.msg, sizeof(status_.msg), "%s", msg);
    if (state == UpdateState::Queued || state == UpdateState::Idle ||
        state == UpdateState::Error || state == UpdateState::Done ||
        state == UpdateState::Rebooting) {
        status_.otaStage[0] = '\0';
        status_.otaStageProgress = 0U;
    }
    portEXIT_CRITICAL(&lock_);

    const bool otaActive = state == UpdateState::Queued ||
                           state == UpdateState::Downloading ||
                           state == UpdateState::Flashing ||
                           state == UpdateState::Rebooting;
    setHmiOtaCondition_(otaActive);
}

void FirmwareUpdateModule::setOtaStage_(const char* stage, uint8_t phaseProgress)
{
    // Two distinct progress axes: existing overall flashed bytes and the
    // current HTTP-download/flash-write phase. All readers see a coherent
    // snapshot via statusJson_'s existing critical section.
    portENTER_CRITICAL(&lock_);
    // A null stage advances the active image's write percentage without
    // changing its mode (PSRAM replay versus concurrent HTTP streaming).
    if (stage) snprintf(status_.otaStage, sizeof(status_.otaStage), "%s", stage);
    status_.otaStageProgress = phaseProgress > 100U ? 100U : phaseProgress;
    status_.updatedAtMs = millis();
    portEXIT_CRITICAL(&lock_);
}

void FirmwareUpdateModule::setError_(FirmwareUpdateTarget target, const char* msg, uint32_t operationId)
{
    // Preserve phase BEFORE UpdateState::Error clears the visible otaStage.
    // Errors are rare and occur after transfer/flash failure; no per-chunk
    // writes to NVS are added.
    char stage[sizeof(status_.otaStage)]{};
    portENTER_CRITICAL(&lock_);
    snprintf(stage, sizeof(stage), "%s",
             status_.otaStage[0] ? status_.otaStage : stateStr_(status_.state));
    portEXIT_CRITICAL(&lock_);
    setStatus_(UpdateState::Error, target, 0, msg ? msg : "failed", operationId);
    persistLastFailure_(target, operationId, stage, msg);
}

void FirmwareUpdateModule::loadLastFailure_()
{
    if (!cfgStore_) return;
    OtaFailure::Record record{};
    size_t length = 0U;
    if (!cfgStore_->readRuntimeBlob(NvsKeys::FirmwareUpdate::LastFailure,
                                    &record, sizeof(record), &length))
        return;
    if (length != sizeof(record) || !OtaFailure::valid(record)) {
        LOGW("[OTA-LASTFAIL] persisted record invalid; ignored");
        return;
    }
    portENTER_CRITICAL(&lock_);
    lastFailure_ = record;
    hasLastFailure_ = true;
    portEXIT_CRITICAL(&lock_);
    LOGW("[OTA-LASTFAIL] retained=1 op=%lu code=%u target=%u stage=%.27s",
         (unsigned long)record.operationId, (unsigned)record.code,
         (unsigned)record.target, record.stage);
    LOGW("[OTA-LASTFAIL] reason=%.95s", record.reason);
}

void FirmwareUpdateModule::persistLastFailure_(
    FirmwareUpdateTarget target, uint32_t operationId,
    const char* stage, const char* reason)
{
    if (!cfgStore_ || operationId == 0U) return;
    const OtaFailure::Record record = OtaFailure::make(
        (uint8_t)target, operationId, stage, reason);
    // A single operation can signal an error from both failLocalRelease_()
    // and runJob_(). Retain the FIRST error once per operation.
    portENTER_CRITICAL(&lock_);
    if (hasLastFailure_ && lastFailure_.operationId == operationId) {
        portEXIT_CRITICAL(&lock_);
        return;
    }
    lastFailure_ = record;
    hasLastFailure_ = true;
    portEXIT_CRITICAL(&lock_);

    // Rare error path only. An OTA success NEVER clears or overwrites this.
    const bool saved = cfgStore_->writeRuntimeBlob(
        NvsKeys::FirmwareUpdate::LastFailure, &record, sizeof(record));
    LOGW("[OTA-LASTFAIL] persisted=%u op=%lu code=%u target=%u stage=%.27s",
         saved ? 1U : 0U, (unsigned long)record.operationId,
         (unsigned)record.code, (unsigned)record.target, record.stage);
    LOGW("[OTA-LASTFAIL] reason=%.95s", record.reason);
}

bool FirmwareUpdateModule::loadReceipt_()
{
    if (!cfgStore_) return false;

    FirmwareUpdateReceipt receipt{};
    size_t actualLen = 0U;
    if (!cfgStore_->readRuntimeBlob(NvsKeys::FirmwareUpdate::Receipt,
                                    &receipt,
                                    sizeof(receipt),
                                    &actualLen)) {
        if (actualLen != 0U) {
            LOGW("Invalid update receipt length=%u; removing it", (unsigned)actualLen);
            (void)cfgStore_->eraseKey(NvsKeys::FirmwareUpdate::Receipt);
        }
        return false;
    }
    if (actualLen != sizeof(receipt) || !firmwareUpdateReceiptIsValid(receipt)) {
        LOGW("Invalid update receipt; removing it");
        (void)cfgStore_->eraseKey(NvsKeys::FirmwareUpdate::Receipt);
        return false;
    }

    if (finalizeFirmwareUpdateReceiptAfterBoot(&receipt) &&
        !cfgStore_->writeRuntimeBlob(NvsKeys::FirmwareUpdate::Receipt,
                                     &receipt,
                                     sizeof(receipt))) {
        LOGE("Failed to finalize update receipt after boot operation_id=%lu",
             (unsigned long)receipt.operationId);
    }

    portENTER_CRITICAL(&lock_);
    lastReceipt_ = receipt;
    hasLastReceipt_ = true;
    portEXIT_CRITICAL(&lock_);
    return true;
}

bool FirmwareUpdateModule::persistReceipt_(FirmwareUpdateTarget target,
                                           uint32_t operationId,
                                           FirmwareUpdateReceiptState state)
{
    if (!cfgStore_ || operationId == 0U) return false;
    const FirmwareUpdateReceipt receipt = makeFirmwareUpdateReceipt(target, operationId, state);
    if (!cfgStore_->writeRuntimeBlob(NvsKeys::FirmwareUpdate::Receipt,
                                     &receipt,
                                     sizeof(receipt))) {
        LOGE("Failed to persist update receipt operation_id=%lu state=%s",
             (unsigned long)operationId,
             firmwareUpdateReceiptStateName(state));
        return false;
    }

    portENTER_CRITICAL(&lock_);
    lastReceipt_ = receipt;
    hasLastReceipt_ = true;
    portEXIT_CRITICAL(&lock_);
    return true;
}

void FirmwareUpdateModule::setHmiOtaCondition_(bool active)
{
    if (hmiOtaActive_ == active) return;
    if (!hmiSvc_ && services_) {
        hmiSvc_ = services_->get<HmiService>(ServiceId::Hmi);
    }
    if (hmiSvc_ && hmiSvc_->setLedCondition) {
        (void)hmiSvc_->setLedCondition(hmiSvc_->ctx, HmiLedCondition::OtaInProgress, active);
    }
    hmiOtaActive_ = active;
}

void FirmwareUpdateModule::onProgressChunk_(uint32_t chunkBytes)
{
    portENTER_CRITICAL(&lock_);
    if (activeTotalBytes_ == 0) {
        portEXIT_CRITICAL(&lock_);
        return;
    }
    uint32_t next = activeSentBytes_ + chunkBytes;
    if (next > activeTotalBytes_) next = activeTotalBytes_;
    activeSentBytes_ = next;
    status_.progress = (uint8_t)((activeSentBytes_ * 100U) / activeTotalBytes_);
    status_.updatedAtMs = millis();
    portEXIT_CRITICAL(&lock_);
}

void FirmwareUpdateModule::attachWebInterfaceSvcIfNeeded_()
{
    if (webInterfaceSvc_ || !services_) return;
    webInterfaceSvc_ = services_->get<WebInterfaceService>(ServiceId::WebInterface);
}

bool FirmwareUpdateModule::resolveUrl_(FirmwareUpdateTarget target,
                                       const char* explicitUrl,
                                       char* out,
                                       size_t outLen,
                                       char* errOut,
                                       size_t errOutLen) const
{
    if (!out || outLen == 0) return false;
    out[0] = '\0';

    if (explicitUrl && explicitUrl[0] != '\0') {
        const int n = snprintf(out, outLen, "%s", explicitUrl);
        if (n <= 0 || (size_t)n >= outLen) {
            writeSimpleError_(errOut, errOutLen, "url too long");
            return false;
        }
        return true;
    }

    (void)target;
    writeSimpleError_(errOut, errOutLen, "url required");
    return false;
}

bool FirmwareUpdateModule::resolveUpdateUrl_(const char* path,
                                             char* out,
                                             size_t outLen,
                                             char* errOut,
                                             size_t errOutLen) const
{
    if (!out || outLen == 0) return false;
    out[0] = '\0';

    if (cfgData_.updateHost[0] == '\0') {
        writeSimpleError_(errOut, errOutLen, "update_host empty");
        return false;
    }
    if (!path || path[0] == '\0') {
        writeSimpleError_(errOut, errOutLen, "path empty");
        return false;
    }

    const bool hasProto =
        (strncmp(cfgData_.updateHost, "http://", 7) == 0) || (strncmp(cfgData_.updateHost, "https://", 8) == 0);
    const int n = hasProto
                      ? snprintf(out, outLen, "%s", cfgData_.updateHost)
                      : snprintf(out, outLen, "http://%s", cfgData_.updateHost);
    if (n <= 0 || (size_t)n >= outLen ||
        !appendUrlSegment_(out, outLen, cfgData_.updatePath) ||
        !appendUrlSegment_(out, outLen, path)) {
        writeSimpleError_(errOut, errOutLen, "resolved url too long");
        return false;
    }
    return true;
}

bool FirmwareUpdateModule::parseUrlArg_(const CommandRequest& req, char* out, size_t outLen) const
{
    if (!out || outLen == 0) return false;
    out[0] = '\0';

    StaticJsonDocument<256> doc;
    if (parseReqJsonObject_(req.args, doc)) {
        const char* url = doc["url"].as<const char*>();
        if (url && url[0] != '\0') {
            snprintf(out, outLen, "%s", url);
            return true;
        }
    }

    doc.clear();
    if (parseReqJsonObject_(req.json, doc)) {
        const char* rootUrl = doc["url"].as<const char*>();
        if (rootUrl && rootUrl[0] != '\0') {
            snprintf(out, outLen, "%s", rootUrl);
            return true;
        }
        JsonVariantConst args = doc["args"];
        if (args.is<JsonObjectConst>()) {
            const char* nestedUrl = args["url"].as<const char*>();
            if (nestedUrl && nestedUrl[0] != '\0') {
                snprintf(out, outLen, "%s", nestedUrl);
                return true;
            }
        }
    }

    return false;
}

bool FirmwareUpdateModule::statusJson_(char* out, size_t outLen)
{
    if (!out || outLen == 0) return false;

    UpdateStatus snap{};
    bool busy = false;
    bool pending = false;
    bool hasLastReceipt = false;
    FirmwareUpdateReceipt lastReceipt{};
    bool hasLastFailure = false;
    OtaFailure::Record lastFailure{};
    portENTER_CRITICAL(&lock_);
    snap = status_;
    busy = busy_ || (localReleaseActive_ && status_.state != UpdateState::Error);
    pending = queuedJob_.pending;
    hasLastReceipt = hasLastReceipt_;
    lastReceipt = lastReceipt_;
    hasLastFailure = hasLastFailure_;
    lastFailure = lastFailure_;
    portEXIT_CRITICAL(&lock_);

    sanitizeJsonString_(snap.msg);

    StaticJsonDocument<640> doc;
    doc["ok"] = true;
    doc["boot_id"] = bootId_;
    doc["operation_id"] = snap.operationId;
    doc["state"] = stateStr_(snap.state);
    doc["target"] = targetStr_(snap.target);
    doc["busy"] = busy;
    doc["pending"] = pending;
    doc["progress"] = snap.progress;
    if (snap.otaStage[0] != '\0') {
        doc["ota_stage"] = snap.otaStage;
        doc["ota_stage_progress"] = snap.otaStageProgress;
    }
    doc["ts_ms"] = snap.updatedAtMs;
    doc["msg"] = snap.msg;
    // Boot evidence is necessary to distinguish an old slot/rollback from
    // an OTA status receipt carried across a restart.
    const esp_partition_t* activeApp = esp_ota_get_running_partition();
    const esp_partition_t* bootApp = esp_ota_get_boot_partition();
    doc["app_slot"] = activeApp ? activeApp->label : "unknown";
    doc["boot_slot"] = bootApp ? bootApp->label : "unknown";
    // Record ESP-IDF reset cause after an interrupted transaction. In
    // particular this distinguishes a watchdog/panic reset from a clean
    // software reboot and does not depend on persistent serial logs.
    doc["reset_reason"] = (unsigned)esp_reset_reason();
    doc["spiffs_valid"] = ReleaseStorage::validateReleaseFilesystem();
    doc["migration_pending"] = ReleaseStorage::legacyPairedMigrationNeeded();
    if (hasLastReceipt) {
        JsonObject receipt = doc.createNestedObject("last_operation");
        receipt["operation_id"] = lastReceipt.operationId;
        receipt["target"] = targetStr_(lastReceipt.target);
        receipt["result"] = firmwareUpdateReceiptStateName(lastReceipt.state);
    } else {
        doc["last_operation"] = nullptr;
    }
    // Optional persistent diagnostic for the 640-byte Web status endpoint.
    // Do not break 512-byte command replies, or even the 640-byte endpoint,
    // if insufficient JSON capacity/output space remains.
    if (hasLastFailure && outLen >= 640U &&
        doc.capacity() > doc.memoryUsage() +
            JSON_OBJECT_SIZE(1) + JSON_OBJECT_SIZE(4) +
            sizeof(lastFailure.stage) + sizeof(lastFailure.reason)) {
        JsonObject last = doc.createNestedObject("last_failure");
        last["operation_id"] = lastFailure.operationId;
        last["code"] = lastFailure.code;
        last["stage"] = lastFailure.stage;
        last["reason"] = lastFailure.reason;
        if (measureJson(doc) >= outLen) doc.remove("last_failure");
    }
    // Command-bus callers may still provide a 512-byte reply. The Web API
    // reserves 640 bytes, but for smaller callers omit the optional phase
    // fields instead of failing an otherwise valid status response.
    if (doc.overflowed()) return false;
    if (measureJson(doc) >= outLen && snap.otaStage[0] != '\0') {
        doc.remove("ota_stage");
        doc.remove("ota_stage_progress");
    }
    if (measureJson(doc) >= outLen) return false;
    const size_t written = serializeJson(doc, out, outLen);
    return written > 0U && written < outLen;
}

bool FirmwareUpdateModule::isBusy_()
{
    if (!manifestMetadata_) return false;

    bool busy = false;
    portENTER_CRITICAL(&lock_);
    // Preserve a failed upload record for diagnostics without blocking
    // Modbus indefinitely. Actual running OTA work is still gated by busy_.
    const bool localReleaseBusy = localReleaseActive_ &&
                                  status_.state != UpdateState::Error;
    busy = busy_ || localReleaseBusy || queuedJob_.pending ||
           nextionRebootQueued_ || updateStartPending_ ||
           manifestCheckIsActive_(manifestMetadata_->check.state);
    portEXIT_CRITICAL(&lock_);
    return busy;
}

bool FirmwareUpdateModule::configJson_(char* out, size_t outLen) const
{
    if (!out || outLen == 0) return false;

    char host[sizeof(cfgData_.updateHost)] = {0};
    char updatePath[sizeof(cfgData_.updatePath)] = {0};
    snprintf(host, sizeof(host), "%s", cfgData_.updateHost);
    snprintf(updatePath, sizeof(updatePath), "%s", cfgData_.updatePath);
    sanitizeJsonString_(host);
    sanitizeJsonString_(updatePath);

    const int n = snprintf(out,
                           outLen,
                           "{\"ok\":true,\"update_host\":\"%s\",\"update_path\":\"%s\"}",
                           host,
                           updatePath);
    return n > 0 && (size_t)n < outLen;
}

bool FirmwareUpdateModule::startManifestCheck_(uint32_t* requestIdOut,
                                               char* errOut,
                                               size_t errOutLen)
{
    if (!requestIdOut) {
        writeSimpleError_(errOut, errOutLen, "request id output missing");
        return false;
    }
    *requestIdOut = 0;

    char url[kUrlLen] = {0};
    if (!resolveUpdateUrl_("manifest.json", url, sizeof(url), errOut, errOutLen)) {
        return false;
    }

    if (!manifestMetadata_ || !manifestPayload_) {
        writeSimpleError_(errOut, errOutLen, "manifest storage unavailable");
        return false;
    }

    portENTER_CRITICAL(&lock_);
    if (busy_ || localReleaseActive_ || queuedJob_.pending || nextionRebootQueued_ || updateStartPending_ ||
        manifestCheckIsActive_(manifestMetadata_->check.state) || manifestCopyReaders_ > 0U) {
        portEXIT_CRITICAL(&lock_);
        writeSimpleError_(errOut, errOutLen, "updater busy");
        return false;
    }

    ++nextManifestRequestId_;
    if (nextManifestRequestId_ == 0U) {
        ++nextManifestRequestId_;
    }
    manifestCheckJob_ = {};
    manifestCheckJob_.pending = true;
    manifestCheckJob_.requestId = nextManifestRequestId_;
    snprintf(manifestCheckJob_.url, sizeof(manifestCheckJob_.url), "%s", url);

    manifestMetadata_->check = {};
    manifestMetadata_->selection = {};
    nextionRecoveryArtifactCount_ = 0U;
    manifestMetadata_->check.requestId = nextManifestRequestId_;
    manifestMetadata_->check.state = FirmwareManifestCheckState::Queued;
    manifestMetadata_->check.updatedAtMs = millis();
    snprintf(manifestMetadata_->check.manifestUrl, sizeof(manifestMetadata_->check.manifestUrl), "%s", url);
    snprintf(manifestMetadata_->check.message, sizeof(manifestMetadata_->check.message), "queued");
    manifestPayload_[0] = '\0';
    *requestIdOut = nextManifestRequestId_;
    portEXIT_CRITICAL(&lock_);

    LOGI("Manifest check queued request=%lu url=%s",
         (unsigned long)*requestIdOut,
         url);
    return true;
}

bool FirmwareUpdateModule::manifestCheckStatus_(uint32_t requestId,
                                                FirmwareManifestCheckSnapshot* out)
{
    if (!manifestMetadata_ || !out || requestId == 0U) return false;
    portENTER_CRITICAL(&lock_);
    if (manifestMetadata_->check.requestId != requestId) {
        portEXIT_CRITICAL(&lock_);
        return false;
    }
    *out = manifestMetadata_->check;
    portEXIT_CRITICAL(&lock_);
    return true;
}

bool FirmwareUpdateModule::copyManifestResult_(uint32_t requestId,
                                               char* out,
                                               size_t outLen,
                                               size_t* copiedLenOut)
{
    if (copiedLenOut) *copiedLenOut = 0U;
    if (!manifestMetadata_ || !out || outLen == 0U || requestId == 0U || !manifestPayload_) return false;

    size_t payloadLen = 0U;
    portENTER_CRITICAL(&lock_);
    if (manifestMetadata_->check.requestId != requestId ||
        manifestMetadata_->check.state != FirmwareManifestCheckState::Ready ||
        manifestMetadata_->check.payloadLen == 0U ||
        outLen <= manifestMetadata_->check.payloadLen) {
        portEXIT_CRITICAL(&lock_);
        return false;
    }
    payloadLen = manifestMetadata_->check.payloadLen;
    if (manifestCopyReaders_ < UINT8_MAX) {
        ++manifestCopyReaders_;
    } else {
        portEXIT_CRITICAL(&lock_);
        return false;
    }
    portEXIT_CRITICAL(&lock_);

    memcpy(out, manifestPayload_, payloadLen);
    out[payloadLen] = '\0';

    portENTER_CRITICAL(&lock_);
    if (manifestCopyReaders_ > 0U) --manifestCopyReaders_;
    portEXIT_CRITICAL(&lock_);

    if (copiedLenOut) *copiedLenOut = payloadLen;
    return true;
}

bool FirmwareUpdateModule::setConfig_(const char* updateHost,
                                      const char* updatePath,
                                      char* errOut,
                                      size_t errOutLen)
{
    if (!manifestMetadata_) {
        writeSimpleError_(errOut, errOutLen, "manifest metadata unavailable");
        return false;
    }

    if (!cfgStore_) {
        writeSimpleError_(errOut, errOutLen, "config store unavailable");
        return false;
    }

    bool isBusy = false;
    bool hasPending = false;
    bool manifestCheckActive = false;
    bool updateStartPending = false;
    portENTER_CRITICAL(&lock_);
    isBusy = busy_;
    hasPending = queuedJob_.pending;
    manifestCheckActive = manifestCheckIsActive_(manifestMetadata_->check.state);
    updateStartPending = updateStartPending_;
    portEXIT_CRITICAL(&lock_);
    if (isBusy || hasPending || manifestCheckActive || updateStartPending) {
        writeSimpleError_(errOut, errOutLen, "updater busy");
        return false;
    }

    if (updateHost) {
        if (!cfgStore_->set(updateHostVar_, updateHost)) {
            writeSimpleError_(errOut, errOutLen, "set update_host failed");
            return false;
        }
    }
    if (updatePath) {
        if (!cfgStore_->set(updatePathVar_, updatePath)) {
            writeSimpleError_(errOut, errOutLen, "set update_path failed");
            return false;
        }
    }

    return true;
}

bool FirmwareUpdateModule::saveCountersBeforeUpdate_(char* errOut, size_t errOutLen)
{
    if (saveCounterCheckpoint(services_)) return true;
    writeSimpleError_(errOut, errOutLen, "counter checkpoint failed; update/reboot cancelled");
    return false;
}

bool FirmwareUpdateModule::startUpdate_(FirmwareUpdateTarget target,
                                        const char* url,
                                        uint32_t* operationIdOut,
                                        char* errOut,
                                        size_t errOutLen)
{
    if (operationIdOut) *operationIdOut = 0U;
    if (!manifestMetadata_) {
        writeSimpleError_(errOut, errOutLen, "manifest metadata unavailable");
        return false;
    }
    UpdateJob job{};
    job.target = target;
    if (target == FirmwareUpdateTarget::Nextion) {
        NextionArtifactSelection selection{};
        portENTER_CRITICAL(&lock_);
        selection = manifestMetadata_->selection;
        if (!selection.valid && url && url[0] != '\0' &&
            manifestMetadata_->check.state == FirmwareManifestCheckState::Ready) {
            for (size_t i = 0U; i < nextionRecoveryArtifactCount_; ++i) {
                if (strcmp(url, manifestMetadata_->recoveryArtifacts[i].url) == 0) {
                    selection = manifestMetadata_->recoveryArtifacts[i];
                    break;
                }
            }
        }
        portEXIT_CRITICAL(&lock_);

        if (!selection.valid) {
            writeSimpleError_(errOut, errOutLen, "no compatible nextion artifact selected");
            return false;
        }
        if (url && url[0] != '\0' && strcmp(url, selection.url) != 0) {
            writeSimpleError_(errOut, errOutLen, "nextion artifact does not match manifest selection");
            return false;
        }

        if (!hmiSvc_ && services_) {
            hmiSvc_ = services_->get<HmiService>(ServiceId::Hmi);
        }
        HmiDisplayIdentity identity{};
        const bool displayDetected =
            hmiSvc_ && hmiSvc_->getLocalDisplayIdentity &&
            hmiSvc_->getLocalDisplayIdentity(hmiSvc_->ctx, &identity);
        if (displayDetected && !isNextionDisplayCompatible(identity, selection.compatibility)) {
            writeSimpleError_(errOut, errOutLen, "selected nextion artifact is incompatible with detected display");
            return false;
        }

        snprintf(job.url, sizeof(job.url), "%s", selection.url);
        snprintf(job.nextionCompatibility,
                 sizeof(job.nextionCompatibility),
                 "%s",
                 selection.compatibility);
        job.expectedSize = selection.size;
        job.nextionHasSha256 = selection.hasSha256;
        if (selection.hasSha256) memcpy(job.nextionSha256, selection.sha256, sizeof(job.nextionSha256));
    } else if (target == FirmwareUpdateTarget::Spiffs ||
               target == FirmwareUpdateTarget::Waveshare) {
        // The old browser sends .../flowios3-spiffs-VERSION.bin, and the
        // older firmware button sends .../flowios3-VERSION.bin. In this
        // engine those names select a VERSIONED paired release manifest.
        // After migration, only the grouped firmware button is presented.
        if (target == FirmwareUpdateTarget::Spiffs &&
            !ReleaseStorage::legacyPairedMigrationNeeded()) {
            return writeSimpleError_(errOut, errOutLen,
                                     "standalone SPIFFS update is forbidden after migration");
        }
        if (!url || !url[0]) {
            if (target == FirmwareUpdateTarget::Spiffs) {
                return writeSimpleError_(errOut, errOutLen, "SPIFFS migration requires versioned URL");
            }
            if (!resolveUpdateUrl_("release.json", job.url, sizeof(job.url),
                                   errOut, errOutLen)) return false;
        } else {
            const size_t length = strlen(url);
            if (strncmp(url, "http://", 7) != 0 &&
                strncmp(url, "https://", 8) != 0) {
                return writeSimpleError_(errOut, errOutLen, "invalid release URL scheme");
            }
            const char* finalSlash = strrchr(url, '/');
            const char* filename = finalSlash ? finalSlash + 1 : nullptr;
            if (!filename || !isSimpleArtifactFilename_(filename)) {
                return writeSimpleError_(errOut, errOutLen, "invalid release filename");
            }
            const char* legacyPrefix = target == FirmwareUpdateTarget::Spiffs
                ? "flowios3-spiffs-" : "flowios3-";
            const size_t legacyPrefixLen = strlen(legacyPrefix);
            const size_t filenameLen = strlen(filename);
            const bool legacyBin =
                strncmp(filename, legacyPrefix, legacyPrefixLen) == 0 &&
                filenameLen > legacyPrefixLen + 4U &&
                strcmp(filename + filenameLen - 4U, ".bin") == 0;
            if (legacyBin) {
                const size_t versionLen = filenameLen - legacyPrefixLen - 4U;
                const char* version = filename + legacyPrefixLen;
                if (target == FirmwareUpdateTarget::Spiffs &&
                    (strlen(FirmwareVersion::Core) != versionLen ||
                     strncmp(version, FirmwareVersion::Core, versionLen) != 0)) {
                    return writeSimpleError_(errOut, errOutLen,
                        "SPIFFS migration version must match installed transition firmware");
                }
                const size_t prefixLen = (size_t)(filename - url);
                const int n = snprintf(job.url, sizeof(job.url),
                    "%.*sflowio-release-%.*s.json",
                    (int)prefixLen, url, (int)versionLen, version);
                if (n <= 0 || (size_t)n >= sizeof(job.url)) {
                    return writeSimpleError_(errOut, errOutLen, "release manifest URL too long");
                }
            } else if ((strcmp(filename, "release.json") == 0 ||
                       (strncmp(filename, "flowio-release-", 15) == 0 &&
                        filenameLen > 20U &&
                        strcmp(filename + filenameLen - 5U, ".json") == 0)) &&
                        target == FirmwareUpdateTarget::Waveshare) {
                if (length >= sizeof(job.url)) {
                    return writeSimpleError_(errOut, errOutLen, "release URL too long");
                }
                memcpy(job.url, url, length + 1U);
            } else {
                return writeSimpleError_(errOut, errOutLen,
                    "expected versioned paired release or old firmware/SPIFFS BIN URL");
            }
        }
    } else if (!resolveUrl_(target, url, job.url, sizeof(job.url), errOut, errOutLen)) {
        return false;
    }

    portENTER_CRITICAL(&lock_);
    if (busy_ || localReleaseActive_ || queuedJob_.pending || nextionRebootQueued_ || updateStartPending_ ||
        manifestCheckIsActive_(manifestMetadata_->check.state) || manifestCopyReaders_ > 0U) {
        portEXIT_CRITICAL(&lock_);
        writeSimpleError_(errOut, errOutLen, "updater busy");
        return false;
    }
    updateStartPending_ = true;
    job.operationId = nextOperationId_++;
    if (nextOperationId_ == 0U) nextOperationId_ = 1U;
    portEXIT_CRITICAL(&lock_);

    if (!saveCountersBeforeUpdate_(errOut, errOutLen)) {
        portENTER_CRITICAL(&lock_);
        updateStartPending_ = false;
        portEXIT_CRITICAL(&lock_);
        return false;
    }
    if (!persistReceipt_(target, job.operationId, FirmwareUpdateReceiptState::Running)) {
        portENTER_CRITICAL(&lock_);
        updateStartPending_ = false;
        portEXIT_CRITICAL(&lock_);
        writeSimpleError_(errOut, errOutLen, "failed to persist update operation");
        return false;
    }

    setStatus_(UpdateState::Queued, target, 0, "queued", job.operationId);
    portENTER_CRITICAL(&lock_);
    queuedJob_ = job;
    queuedJob_.pending = true;
    updateStartPending_ = false;
    portEXIT_CRITICAL(&lock_);

    if (operationIdOut) *operationIdOut = job.operationId;
    LOGI("Update queued operation_id=%lu target=%s url=%s",
         (unsigned long)job.operationId,
         targetStr_(target),
         job.url);
    return true;
}

bool FirmwareUpdateModule::queueNextionReboot_(char* errOut, size_t errOutLen)
{
    if (!manifestMetadata_) {
        writeSimpleError_(errOut, errOutLen, "manifest metadata unavailable");
        return false;
    }

    if (nextionRebootPin_ < 0) {
        writeSimpleError_(errOut, errOutLen, "nextion reboot pin not configured");
        return false;
    }

    portENTER_CRITICAL(&lock_);
    if (busy_ || localReleaseActive_ || queuedJob_.pending || nextionRebootQueued_ || updateStartPending_ ||
        manifestCheckIsActive_(manifestMetadata_->check.state) || manifestCopyReaders_ > 0U) {
        portEXIT_CRITICAL(&lock_);
        writeSimpleError_(errOut, errOutLen, "updater busy");
        return false;
    }
    nextionRebootQueued_ = true;
    portEXIT_CRITICAL(&lock_);

    LOGI("Nextion reboot queued");
    return true;
}

bool FirmwareUpdateModule::runWaveshareUpdate_(const UpdateJob& job, char* errOut, size_t errOutLen)
{
    // Remote OTA is a single, verified release transaction: both images are
    // downloaded from one manifest and committed with ONE reboot.
    setStatus_(UpdateState::Downloading, FirmwareUpdateTarget::Waveshare,
               0U, "downloading paired release manifest", job.operationId);
    RebootDiagnostics::noteOta(job.operationId, RebootDiagnostics::OtaStage::Manifest);
    LOGI("[OTA-TRACE] op=%lu stage=manifest_start", (unsigned long)job.operationId);
    setOtaStage_("manifest", 0U);
    otaMonitor_("manifest_start");

    constexpr size_t kManifestCapacity = 1024U;
    // Keep the manifest buffer off the 6 KiB updater task stack.
    std::unique_ptr<char, decltype(&heap_caps_free)> manifest(
        static_cast<char*>(heap_caps_calloc(kManifestCapacity + 1U, 1U,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        &heap_caps_free);
    if (!manifest) return writeSimpleError_(errOut, errOutLen, "paired release manifest memory unavailable");
    // Even the small release manifest receives up to three attempts.
    // Restart from zero rather than requesting a partial JSON document.
    size_t manifestLen = 0U;
    bool manifestReady = false;
    bool manifestPermanentError = false;
    for (unsigned attempt = 1U;
         !manifestReady && !manifestPermanentError &&
         attempt <= OtaHttpRetryPolicy::kMaxAttempts; ++attempt) {
        LOGI("[OTA-HTTP] op=%lu target=manifest attempt=%u/%u offset=0",
             (unsigned long)job.operationId, attempt, OtaHttpRetryPolicy::kMaxAttempts);
        HTTPClient manifestHttp;
        configureDownloadHttp_(manifestHttp);
        if (!manifestHttp.begin(job.url)) {
            LOGW("[OTA-HTTP] op=%lu target=manifest attempt=%u stage=begin_failed",
                 (unsigned long)job.operationId, attempt);
        } else {
            const int code = manifestHttp.GET();
            const int manifestSize = manifestHttp.getSize();
            if (code == HTTP_CODE_OK && manifestSize > 0 &&
                manifestSize <= (int)kManifestCapacity) {
                BoundedBufferStream sink(manifest.get(), kManifestCapacity);
                const int manifestRead = manifestHttp.writeToStream(&sink);
                manifestLen = sink.length();
                manifestReady = !sink.overflowed() && manifestRead >= 0 &&
                                manifestLen == (size_t)manifestSize;
            } else if (!OtaHttpRetryPolicy::isRetryableStatus(code)) {
                // A genuine 404, bad authorization or malformed manifest
                // length will not become valid merely by retrying.
                manifestPermanentError = true;
            }
            LOGI("[OTA-HTTP] op=%lu target=manifest attempt=%u code=%d length=%d received=%lu valid=%u",
                 (unsigned long)job.operationId, attempt, code, manifestSize,
                 (unsigned long)manifestLen, manifestReady ? 1U : 0U);
            manifestHttp.end();
        }
        if (!manifestReady && !manifestPermanentError &&
            attempt < OtaHttpRetryPolicy::kMaxAttempts) {
            LOGW("[OTA-HTTP] op=%lu target=manifest stage=retry_scheduled next=%u/%u",
                 (unsigned long)job.operationId, attempt + 1U,
                 OtaHttpRetryPolicy::kMaxAttempts);
            vTaskDelay(pdMS_TO_TICKS(OtaHttpRetryPolicy::kRetryDelayMs));
        }
    }
    LOGI("[OTA-TRACE] op=%lu stage=manifest_complete got=%lu valid=%u",
         (unsigned long)job.operationId, (unsigned long)manifestLen, manifestReady ? 1U : 0U);
    otaMonitor_("manifest_complete");
    if (manifestReady) setOtaStage_("manifest", 100U);
    if (!manifestReady) {
        return writeSimpleError_(errOut, errOutLen,
                                 "paired release manifest transfer failed after HTTP retries");
    }
    manifest.get()[manifestLen] = '\0';
    LOGI("[OTA] manifest received bytes=%u stack_free_bytes=%u internal_ram=%u",
         (unsigned)manifestLen,
         (unsigned)uxTaskGetStackHighWaterMark(nullptr),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // Complete the first manifest parse in its own scope so that the
    // ArduinoJson document does not overlap nested beginLocalRelease_().
    // JSON capacity is still allocated explicitly in PSRAM.
    uint32_t fsSize = 0U;
    uint32_t fwSize = 0U;
    char fsUrl[kUrlLen]{};
    char fwUrl[kUrlLen]{};
    {
    SpiRamJsonDocument doc(2048U);
    if (doc.capacity() == 0U) {
        return writeSimpleError_(errOut, errOutLen, "paired release manifest json psram unavailable");
    }
    otaMonitor_("paired_manifest_json_psram");
    if (deserializeJson(doc, static_cast<const char*>(manifest.get()), manifestLen) != DeserializationError::Ok ||
        !doc.is<JsonObjectConst>()) {
        return writeSimpleError_(errOut, errOutLen, "paired release manifest invalid JSON");
    }
    // In the transitional SPIFFS-button path the remote release must be
    // exactly the version of the firmware already installed by the first
    // (legacy) click. Do not silently install a different published release.
    const char* releaseVersion = doc["version"] | "";
    LOGI("[OTA] manifest parsed version=%s stack_free_bytes=%u",
         releaseVersion, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    if (job.target == FirmwareUpdateTarget::Spiffs &&
        strcmp(releaseVersion, FirmwareVersion::Core) != 0) {
        return writeSimpleError_(errOut, errOutLen,
                                 "paired release version does not match transition firmware");
    }
    const char* fsFile = doc["filesystem"]["file"] | "";
    const char* fwFile = doc["firmware"]["file"] | "";
    fsSize = doc["filesystem"]["size"] | 0U;
    fwSize = doc["firmware"]["size"] | 0U;
    // Accept the original flat binary/ filenames (flowios3-<version>.bin,
    // flowios3-spiffs-<version>.bin) alongside canonical release filenames.
    // Only simple filenames can be used: no paths, traversal or external URLs.
    if (!isSimpleArtifactFilename_(fsFile) || !isSimpleArtifactFilename_(fwFile) ||
        fsSize == 0U || fwSize == 0U || strcmp(fsFile, fwFile) == 0) {
        return writeSimpleError_(errOut, errOutLen, "paired release artifact filename invalid");
    }
    if (!buildManifestSiblingUrl_(job.url, fsFile, fsUrl, sizeof(fsUrl)) ||
        !buildManifestSiblingUrl_(job.url, fwFile, fwUrl, sizeof(fwUrl))) {
        return writeSimpleError_(errOut, errOutLen, "paired release artifact URL invalid");
    }
    } // release initial JSON tree before nested parsing
    otaMonitor_("paired_manifest_json_released");

    // The worker owns busy_ while this function executes. Permit only this
    // task to start the existing transactional upload implementation.
    portENTER_CRITICAL(&lock_);
    remoteReleaseWorker_ = xTaskGetCurrentTaskHandle();
    remoteReleaseOperationId_ = job.operationId;
    portEXIT_CRITICAL(&lock_);

    // Avoid in-place JSON serialization: ArduinoJson may refer to strings
    // inside the input buffer. Keep the original manifest and let the
    // transaction validate its versioned artifact names for remote OTA only.
    LOGI("[OTA] preparing paired release stack_free_bytes=%u",
         (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    uint32_t transactionId = 0U;
    bool success = beginLocalRelease_(manifest.get(), manifestLen, &transactionId,
                                      errOut, errOutLen);
    if (!success) {
        LOGE("[OTA] paired release preparation failed: %s", errOut ? errOut : "-");
    }
    if (success) {
        LOGI("[OTA] paired release preparation complete stack_free_bytes=%u",
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        LOGI("[OTA] paired release begin id=%lu slot=%s fs=%lu fw=%lu",
             (unsigned long)transactionId,
             ReleaseStorage::inactiveSlot() == ReleaseSlot::A ? "A" : "B",
             (unsigned long)fsSize, (unsigned long)fwSize);
    }

    // Reuse the *same* SHA-256, partition-overlap and release.json checks
    // as the local paired A/B uploader. No second flash algorithm is needed.
    const FirmwareUpdateTarget targets[2] = {
        FirmwareUpdateTarget::Spiffs, FirmwareUpdateTarget::Waveshare
    };
    const char* urls[2] = {fsUrl, fwUrl};
    const uint32_t sizes[2] = {fsSize, fwSize};
    for (size_t imageIndex = 0U; success && imageIndex < 2U; ++imageIndex) {
        const auto target = targets[imageIndex];
        const uint32_t expectedSize = sizes[imageIndex];
        // One 1 KiB INTERNAL stack buffer is shared by the PSRAM replay and
        // legacy streaming paths; do not double OTA task-stack usage.
        uint8_t chunk[1024]{};

        // Experimental preverified PSRAM mode. Download and validate each
        // complete artifact BEFORE erasing/writing its inactive A/B partition.
        // Keep 2 MiB free for Wi-Fi, AsyncTCP, MQTT, web assets and other tasks.
        // If allocation is impossible, use the unchanged 1 KiB streaming path.
        constexpr size_t kRamReserveBytes = OtaPsramPolicy::kReserveBytes;
        constexpr size_t kRamReadBytes = 1024U;
        constexpr size_t kRamHashBytes = 16U * 1024U;
        constexpr size_t kRamProgressBytes = 256U * 1024U;
        const size_t ramFreeBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const size_t ramLargestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        std::unique_ptr<uint8_t, decltype(&heap_caps_free)> ramImage(nullptr, &heap_caps_free);
        const bool enoughRam = OtaPsramPolicy::canStageImage(
            expectedSize, ramFreeBefore, ramLargestBefore);
        if (enoughRam) {
            ramImage.reset(static_cast<uint8_t*>(
                heap_caps_malloc(expectedSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        }
        if (ramImage) {
            const char* targetName = targetStr_(target);
            const uint32_t downloadStartedMs = millis();
            const uint8_t baseProgress = imageIndex == 0U ? 0U
                : (uint8_t)(((uint64_t)fsSize * 100U) / ((uint64_t)fsSize + fwSize));
            size_t received = 0U;
            size_t lastLogged = 0U;
            uint8_t lastUiPercent = 0U;
            uint32_t lastProgressMs = millis();
            uint32_t lastWaitLogMs = lastProgressMs;
            const auto ramStage = imageIndex == 0U
                ? RebootDiagnostics::OtaStage::SpiffsRamDownload
                : RebootDiagnostics::OtaStage::FirmwareRamDownload;
            const auto ramVerifyStage = imageIndex == 0U
                ? RebootDiagnostics::OtaStage::SpiffsRamVerify
                : RebootDiagnostics::OtaStage::FirmwareRamVerify;
            RebootDiagnostics::noteOta(job.operationId, ramStage, 0U, expectedSize);
            // Keep the overall percentage tied to bytes committed to A/B,
            // rather than jumping forward during RAM receipt and then back
            // when flash writing begins. UI remains on the paired OTA track.
            setStatus_(UpdateState::Downloading, FirmwareUpdateTarget::Waveshare,
                       baseProgress, imageIndex == 0U ? "downloading filesystem to PSRAM"
                                                    : "downloading firmware to PSRAM",
                       job.operationId);
            setOtaStage_(imageIndex == 0U ? "spiffs_download" : "firmware_download", 0U);
            LOGI("[OTA-RAM] op=%lu target=%s stage=download_begin mode=psram size=%lu free_before=%lu largest_before=%lu free_after=%lu reserve=%lu",
                 (unsigned long)job.operationId, targetName, (unsigned long)expectedSize,
                 (unsigned long)ramFreeBefore, (unsigned long)ramLargestBefore,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned long)kRamReserveBytes);

            // Three attempts TOTAL for each artifact. Resume via HTTP Range
            // only after validating Content-Range; a server returning HTTP 200
            // instead is treated as a clean restart into the same RAM buffer.
            for (unsigned attempt = 1U;
                 success && received < expectedSize && attempt <= OtaHttpRetryPolicy::kMaxAttempts;
                 ++attempt) {
                const size_t requestedOffset = received;
                bool retryableFailure = false;
                LOGI("[OTA-HTTP] op=%lu target=%s mode=psram attempt=%u/%u offset=%lu",
                     (unsigned long)job.operationId, targetName, attempt,
                     OtaHttpRetryPolicy::kMaxAttempts, (unsigned long)requestedOffset);
                HTTPClient ramHttp;
                configureDownloadHttp_(ramHttp);
                if (!ramHttp.begin(urls[imageIndex])) {
                    retryableFailure = true;
                    LOGW("[OTA-HTTP] op=%lu target=%s attempt=%u stage=begin_failed",
                         (unsigned long)job.operationId, targetName, attempt);
                } else {
                    const char* responseHeaders[] = {"Content-Range"};
                    ramHttp.collectHeaders(responseHeaders, 1U);
                    if (requestedOffset > 0U) {
                        ramHttp.addHeader("Range", String("bytes=") +
                                          String((unsigned long)requestedOffset) + "-");
                    }
                    const int code = ramHttp.GET();
                    const int announced = ramHttp.getSize();
                    const String contentRange = ramHttp.header("Content-Range");
                    const auto mode = OtaHttpRetryPolicy::responseMode(
                        code, requestedOffset, announced > 0 ? (size_t)announced : 0U,
                        expectedSize, contentRange.c_str());
                    LOGI("[OTA-HTTP] op=%lu target=%s attempt=%u code=%d length=%d offset=%lu mode=%u",
                         (unsigned long)job.operationId, targetName, attempt, code,
                         announced, (unsigned long)requestedOffset, (unsigned)mode);
                    if (mode == OtaHttpRetryPolicy::ResponseMode::Invalid) {
                        retryableFailure = OtaHttpRetryPolicy::isRetryableStatus(code);
                        if (!retryableFailure) {
                            success = writeSimpleError_(errOut, errOutLen,
                                "PSRAM artifact HTTP response/Range invalid");
                        }
                    } else {
                        if (mode == OtaHttpRetryPolicy::ResponseMode::Full &&
                            requestedOffset > 0U) {
                            LOGW("[OTA-HTTP] op=%lu target=%s attempt=%u stage=restart_full server_no_range=1",
                                 (unsigned long)job.operationId, targetName, attempt);
                            received = 0U;
                            lastLogged = 0U;
                            lastUiPercent = 0U;
                            setOtaStage_(nullptr, 0U);
                        }
                        Stream* stream = ramHttp.getStreamPtr();
                        lastProgressMs = millis();
                        lastWaitLogMs = lastProgressMs;
                        while (success && !retryableFailure && received < expectedSize) {
                            const size_t available = stream ? stream->available() : 0U;
                            if (available == 0U) {
                                const uint32_t idleMs = millis() - lastProgressMs;
                                if (!ramHttp.connected() ||
                                    idleMs > Limits::FirmwareUpdate::Http::StreamReadTimeoutMs) {
                                    retryableFailure = true;
                                    LOGW("[OTA-HTTP] op=%lu target=%s attempt=%u stage=interrupted received=%lu/%lu idle_ms=%lu connected=%u",
                                         (unsigned long)job.operationId, targetName,
                                         attempt, (unsigned long)received,
                                         (unsigned long)expectedSize, (unsigned long)idleMs,
                                         ramHttp.connected() ? 1U : 0U);
                                    break;
                                }
                                if (millis() - lastWaitLogMs >= 5000U) {
                                    LOGW("[OTA-RAM] op=%lu target=%s stage=http_waiting received=%lu/%lu idle_ms=%lu attempt=%u",
                                         (unsigned long)job.operationId, targetName,
                                         (unsigned long)received, (unsigned long)expectedSize,
                                         (unsigned long)idleMs, attempt);
                                    lastWaitLogMs = millis();
                                }
                                vTaskDelay(1);
                                continue;
                            }
                            size_t amount = available > kRamReadBytes ? kRamReadBytes : available;
                            if (amount > expectedSize - received) amount = expectedSize - received;
                            const int rd = stream->readBytes(
                                reinterpret_cast<char*>(ramImage.get() + received), amount);
                            if (rd <= 0) {
                                if (!ramHttp.connected() ||
                                    millis() - lastProgressMs > Limits::FirmwareUpdate::Http::StreamReadTimeoutMs) {
                                    retryableFailure = true;
                                    LOGW("[OTA-HTTP] op=%lu target=%s attempt=%u stage=read_stalled offset=%lu",
                                         (unsigned long)job.operationId, targetName,
                                         attempt, (unsigned long)received);
                                    break;
                                }
                                vTaskDelay(1);
                                continue;
                            }
                            received += (size_t)rd;
                            lastProgressMs = millis();
                            // Refresh the UI in 1% increments without emitting
                            // extra 1 KiB network logs or status JSON traffic.
                            const uint8_t downloadPercent =
                                (uint8_t)(((uint64_t)received * 100U) / expectedSize);
                            if (downloadPercent != lastUiPercent) {
                                setOtaStage_(nullptr, downloadPercent);
                                lastUiPercent = downloadPercent;
                            }
                            if (received - lastLogged >= kRamProgressBytes ||
                                received == expectedSize) {
                                RebootDiagnostics::noteOta(job.operationId, ramStage,
                                                           (uint32_t)received, expectedSize);
                                setStatus_(UpdateState::Downloading, FirmwareUpdateTarget::Waveshare,
                                           baseProgress,
                                           imageIndex == 0U ? "downloading filesystem to PSRAM"
                                                            : "downloading firmware to PSRAM",
                                           job.operationId);
                                setOtaStage_(imageIndex == 0U ? "spiffs_download" : "firmware_download",
                                    (uint8_t)(((uint64_t)received * 100U) / expectedSize));
                                LOGI("[OTA-RAM] op=%lu target=%s stage=download_progress received=%lu/%lu attempt=%u",
                                     (unsigned long)job.operationId, targetName,
                                     (unsigned long)received, (unsigned long)expectedSize, attempt);
                                lastLogged = received;
                            }
                            vTaskDelay(1);
                        }
                    }
                    // Close the old TCP socket before a retry or flash.
                    ramHttp.end();
                }
                if (success && !retryableFailure && received == expectedSize) break;
                if (!success) break; // Fatal HTTP response; never retry an invalid Range.
                if (!retryableFailure) {
                    success = writeSimpleError_(errOut, errOutLen, "PSRAM artifact incomplete");
                    break;
                }
                if (attempt == OtaHttpRetryPolicy::kMaxAttempts) {
                    LOGE("[OTA-HTTP] op=%lu target=%s stage=retries_exhausted attempts=%u received=%lu/%lu",
                         (unsigned long)job.operationId, targetName,
                         OtaHttpRetryPolicy::kMaxAttempts,
                         (unsigned long)received, (unsigned long)expectedSize);
                    success = writeSimpleError_(errOut, errOutLen,
                                                "PSRAM artifact failed after 3 HTTP attempts");
                    break;
                }
                LOGW("[OTA-HTTP] op=%lu target=%s stage=retry_scheduled next=%u/%u offset=%lu",
                     (unsigned long)job.operationId, targetName, attempt + 1U,
                     OtaHttpRetryPolicy::kMaxAttempts, (unsigned long)received);
                vTaskDelay(pdMS_TO_TICKS(OtaHttpRetryPolicy::kRetryDelayMs));
            }
            const uint32_t downloadMs = millis() - downloadStartedMs;
            if (success) setOtaStage_(imageIndex == 0U ? "spiffs_download" : "firmware_download", 100U);
            LOGI("[OTA-RAM] op=%lu target=%s stage=download_end success=%u bytes=%lu/%lu ms=%lu",
                 (unsigned long)job.operationId, targetName, success ? 1U : 0U,
                 (unsigned long)received, (unsigned long)expectedSize, (unsigned long)downloadMs);

            uint32_t verifyMs = 0U;
            if (success) {
                const uint32_t verifyStarted = millis();
                RebootDiagnostics::noteOta(job.operationId, ramVerifyStage, expectedSize, expectedSize);
                setStatus_(UpdateState::Flashing, FirmwareUpdateTarget::Waveshare,
                           baseProgress, imageIndex == 0U ? "SHA-256 filesystem in PSRAM"
                                                        : "SHA-256 firmware in PSRAM",
                           job.operationId);
                setOtaStage_(imageIndex == 0U ? "spiffs_ram_verify" : "firmware_ram_verify", 0U);
                mbedtls_sha256_context ramSha{};
                mbedtls_sha256_init(&ramSha);
                bool shaOk = mbedtls_sha256_starts(&ramSha, 0) == 0;
                for (size_t offset = 0U; shaOk && offset < expectedSize;) {
                    const size_t length = (expectedSize - offset) > kRamHashBytes
                        ? kRamHashBytes : (expectedSize - offset);
                    shaOk = mbedtls_sha256_update(&ramSha, ramImage.get() + offset, length) == 0;
                    offset += length;
                    vTaskDelay(1);
                }
                uint8_t digest[32]{};
                if (shaOk) shaOk = mbedtls_sha256_finish(&ramSha, digest) == 0;
                mbedtls_sha256_free(&ramSha);
                const uint8_t* expectedSha = imageIndex == 0U
                    ? localRelease_.filesystem.sha256 : localRelease_.firmware.sha256;
                shaOk = shaOk && sha256Matches_(digest, expectedSha);
                verifyMs = millis() - verifyStarted;
                LOGI("[OTA-RAM] op=%lu target=%s stage=preverify_end sha256=%s ms=%lu flash_touched=0",
                     (unsigned long)job.operationId, targetName, shaOk ? "OK" : "FAIL",
                     (unsigned long)verifyMs);
                if (!shaOk) {
                    success = writeSimpleError_(errOut, errOutLen, "PSRAM artifact SHA-256 mismatch");
                } else {
                    setOtaStage_(imageIndex == 0U ? "spiffs_ram_verify" : "firmware_ram_verify", 100U);
                }
            }

            uint32_t prepareMs = 0U;
            uint32_t writeMs = 0U;
            uint32_t maxFlashUs = 0U;
            uint32_t slowFlashWrites = 0U;
            if (success) {
                // Only after complete HTTP + manifest hash validation is the
                // inactive partition erased. Write from a small INTERNAL
                // buffer: PSRAM need not be readable during flash-cache stalls.
                RebootDiagnostics::noteOta(job.operationId,
                    imageIndex == 0U ? RebootDiagnostics::OtaStage::Spiffs
                                     : RebootDiagnostics::OtaStage::Firmware, 0U, expectedSize);
                setStatus_(UpdateState::Flashing, FirmwareUpdateTarget::Waveshare,
                           baseProgress, imageIndex == 0U ? "preparing release filesystem"
                                                        : "preparing release firmware", job.operationId);
                setOtaStage_(imageIndex == 0U ? "spiffs_prepare" : "firmware_prepare", 0U);
                LOGI("[OTA-RAM] op=%lu target=%s stage=flash_prepare_start", (unsigned long)job.operationId, targetName);
                otaMonitor_(imageIndex == 0U ? "spiffs_prepare_start" : "firmware_prepare_start");
                const uint32_t prepareStarted = millis();
                success = beginLocalImage_(transactionId, target, expectedSize, errOut, errOutLen);
                prepareMs = millis() - prepareStarted;
                LOGI("[OTA-RAM] op=%lu target=%s stage=flash_prepare_end success=%u ms=%lu",
                     (unsigned long)job.operationId, targetName, success ? 1U : 0U,
                     (unsigned long)prepareMs);
            }
            if (success) {
                setOtaStage_(imageIndex == 0U ? "spiffs_write" : "firmware_write", 0U);
                const uint32_t writingStarted = millis();
                size_t flashed = 0U;
                size_t lastFlashLog = 0U;
                while (success && flashed < expectedSize) {
                    const size_t amount = (expectedSize - flashed) > sizeof(chunk)
                        ? sizeof(chunk) : (expectedSize - flashed);
                    memcpy(chunk, ramImage.get() + flashed, amount);
                    const uint32_t flashStartUs = micros();
                    success = writeLocalImage_(transactionId, target, chunk, amount,
                                               flashed, errOut, errOutLen);
                    const uint32_t flashUs = (uint32_t)(micros() - flashStartUs);
                    if (flashUs > maxFlashUs) maxFlashUs = flashUs;
                    if (flashUs >= 20000U) ++slowFlashWrites;
                    if (!success) {
                        LOGE("[OTA-RAM] op=%lu target=%s stage=flash_error offset=%lu reason=%s",
                             (unsigned long)job.operationId, targetName,
                             (unsigned long)flashed, errOut ? errOut : "-");
                        break;
                    }
                    flashed += amount;
                    if (flashed - lastFlashLog >= kRamProgressBytes || flashed == expectedSize) {
                        RebootDiagnostics::noteOta(job.operationId,
                            imageIndex == 0U ? RebootDiagnostics::OtaStage::Spiffs
                                             : RebootDiagnostics::OtaStage::Firmware,
                            (uint32_t)flashed, expectedSize);
                        LOGI("[OTA-RAM] op=%lu target=%s stage=flash_progress flashed=%lu/%lu flash_max_us=%lu slow20ms=%lu",
                             (unsigned long)job.operationId, targetName,
                             (unsigned long)flashed, (unsigned long)expectedSize,
                             (unsigned long)maxFlashUs, (unsigned long)slowFlashWrites);
                        otaMonitor_(imageIndex == 0U ? "spiffs_progress" : "firmware_progress");
                        lastFlashLog = flashed;
                    }
                    vTaskDelay(1);
                }
                writeMs = millis() - writingStarted;
            }
            if (success) {
                RebootDiagnostics::noteOta(job.operationId,
                    imageIndex == 0U ? RebootDiagnostics::OtaStage::VerifySpiffs
                                     : RebootDiagnostics::OtaStage::VerifyFirmware,
                    expectedSize, expectedSize);
                setStatus_(UpdateState::Flashing, FirmwareUpdateTarget::Waveshare,
                           imageIndex == 0U ? (uint8_t)(((uint64_t)fsSize * 100U) / ((uint64_t)fsSize + fwSize))
                                            : 99U,
                           imageIndex == 0U ? "verifying release filesystem" : "verifying release firmware",
                           job.operationId);
                setOtaStage_(imageIndex == 0U ? "spiffs_verify" : "firmware_verify", 0U);
                LOGI("[OTA-RAM] op=%lu target=%s stage=image_verify_start",
                     (unsigned long)job.operationId, targetName);
                success = endLocalImage_(transactionId, target, errOut, errOutLen);
                LOGI("[OTA-RAM] op=%lu target=%s stage=image_verify_end success=%u",
                     (unsigned long)job.operationId, targetName, success ? 1U : 0U);
                if (success) setOtaStage_(imageIndex == 0U ? "spiffs_verify" : "firmware_verify", 100U);
            }
            LOGI("[OTA-RAM] op=%lu target=%s stage=image_end mode=psram success=%u download_ms=%lu preverify_ms=%lu flash_prepare_ms=%lu flash_write_ms=%lu flash_max_us=%lu free_psram=%lu",
                 (unsigned long)job.operationId, targetName, success ? 1U : 0U,
                 (unsigned long)downloadMs, (unsigned long)verifyMs,
                 (unsigned long)prepareMs, (unsigned long)writeMs,
                 (unsigned long)maxFlashUs,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            // Loop condition gates the next image and final A/B commit on success.
            // A failed download/hash never touches that image's inactive flash.
            continue;
        }
        LOGW("[OTA-RAM] op=%lu target=%s stage=fallback_stream reason=psram_reserve_or_alloc size=%lu free=%lu largest=%lu reserve=%lu",
             (unsigned long)job.operationId, targetStr_(target),
             (unsigned long)expectedSize, (unsigned long)ramFreeBefore,
             (unsigned long)ramLargestBefore, (unsigned long)kRamReserveBytes);
        // Legacy low-memory path: original HTTP -> 1 KiB flash streaming.
        // Update the UI before a potentially slow partition erase/esp_ota_begin.
        // This stage is informative only; the verified A/B commit is unchanged.
        const uint8_t preparedProgress = imageIndex == 0U ? 0U
            : (uint8_t)(((uint64_t)fsSize * 100U) / ((uint64_t)fsSize + fwSize));
        setStatus_(UpdateState::Flashing, FirmwareUpdateTarget::Waveshare,
                   preparedProgress,
                   imageIndex == 0U ? "preparing release filesystem"
                                    : "preparing release firmware",
                   job.operationId);
        setOtaStage_(imageIndex == 0U ? "spiffs_prepare" : "firmware_prepare", 0U);
        // Prepare the inactive partition BEFORE starting the HTTP transfer,
        // so a lengthy flash erase cannot starve an open TCP download.
        RebootDiagnostics::noteOta(job.operationId,
            imageIndex == 0U ? RebootDiagnostics::OtaStage::Spiffs
                             : RebootDiagnostics::OtaStage::Firmware,
            0U, expectedSize);
        LOGI("[OTA-TRACE] op=%lu target=%s stage=partition_prepare_start",
             (unsigned long)job.operationId, targetStr_(target));
        otaMonitor_(imageIndex == 0U ? "spiffs_prepare_start" : "firmware_prepare_start");
        const uint32_t prepareStartedMs = millis();
        success = beginLocalImage_(transactionId, target, expectedSize, errOut, errOutLen);
        LOGI("[OTA-TRACE] op=%lu target=%s stage=partition_prepare_end ms=%lu success=%u",
             (unsigned long)job.operationId, targetStr_(target),
             (unsigned long)(millis() - prepareStartedMs), success ? 1U : 0U);
        otaMonitor_(imageIndex == 0U ? "spiffs_prepare_end" : "firmware_prepare_end");
        if (!success) {
            LOGE("[OTA] target preparation failed target=%s reason=%s",
                 targetStr_(target), errOut ? errOut : "-");
            break;
        }

        LOGI("[OTA] beginning HTTP artifact target=%s size=%lu",
             targetStr_(target), (unsigned long)expectedSize);
        // Streaming fallback downloads and writes simultaneously, not as
        // independent PSRAM/flash stages. Expose the combined mode explicitly.
        setOtaStage_(imageIndex == 0U ? "spiffs_stream" : "firmware_stream", 0U);
        // Streaming fallback also gets three transport attempts. If Range
        // is unsupported, request the whole file again and DISCARD the already
        // flashed prefix; the existing SHA-256 validator detects any mismatch
        // between the first attempt and the replayed artifact.
        size_t received = 0U;
        size_t lastLoggedBytes = 0U;
        uint32_t maxFlashWriteUs = 0U;
        uint32_t slowFlashWrites = 0U;
        for (unsigned attempt = 1U;
             success && received < expectedSize && attempt <= OtaHttpRetryPolicy::kMaxAttempts;
             ++attempt) {
            const size_t requestedOffset = received;
            size_t replaySkip = 0U;
            bool retryableFailure = false;
            LOGI("[OTA-HTTP] op=%lu target=%s mode=stream attempt=%u/%u offset=%lu",
                 (unsigned long)job.operationId, targetStr_(target), attempt,
                 OtaHttpRetryPolicy::kMaxAttempts, (unsigned long)requestedOffset);
            HTTPClient artifactHttp;
            configureDownloadHttp_(artifactHttp);
            if (!artifactHttp.begin(urls[imageIndex])) {
                retryableFailure = true;
                LOGW("[OTA-HTTP] op=%lu target=%s attempt=%u stage=begin_failed mode=stream",
                     (unsigned long)job.operationId, targetStr_(target), attempt);
            } else {
                const char* responseHeaders[] = {"Content-Range"};
                artifactHttp.collectHeaders(responseHeaders, 1U);
                if (requestedOffset > 0U) {
                    artifactHttp.addHeader("Range", String("bytes=") +
                                           String((unsigned long)requestedOffset) + "-");
                }
                const int code = artifactHttp.GET();
                const int announced = artifactHttp.getSize();
                const String contentRange = artifactHttp.header("Content-Range");
                const auto mode = OtaHttpRetryPolicy::responseMode(
                    code, requestedOffset, announced > 0 ? (size_t)announced : 0U,
                    expectedSize, contentRange.c_str());
                LOGI("[OTA-HTTP] op=%lu target=%s attempt=%u code=%d length=%d offset=%lu mode=%u stream=1",
                     (unsigned long)job.operationId, targetStr_(target),
                     attempt, code, announced, (unsigned long)requestedOffset, (unsigned)mode);
                if (mode == OtaHttpRetryPolicy::ResponseMode::Invalid) {
                    retryableFailure = OtaHttpRetryPolicy::isRetryableStatus(code);
                    if (!retryableFailure) {
                        success = writeSimpleError_(errOut, errOutLen,
                            "streaming artifact HTTP response/Range invalid");
                    }
                } else {
                    if (mode == OtaHttpRetryPolicy::ResponseMode::Full &&
                        requestedOffset > 0U) {
                        // HTTP 200: the server has ignored Range. Read and
                        // discard the prefix without writing it again.
                        // Final manifest SHA-256 still verifies all A/B data.
                        replaySkip = requestedOffset;
                        LOGW("[OTA-HTTP] op=%lu target=%s attempt=%u stage=restart_full_skip bytes=%lu",
                             (unsigned long)job.operationId, targetStr_(target),
                             attempt, (unsigned long)replaySkip);
                    }
                    Stream* stream = artifactHttp.getStreamPtr();
                    uint32_t lastProgressMs = millis();
                    uint32_t lastLogMs = lastProgressMs;
                    while (success && !retryableFailure &&
                           (received < expectedSize || replaySkip > 0U)) {
                        const size_t available = stream ? stream->available() : 0U;
                        if (available == 0U) {
                            const uint32_t idleMs = millis() - lastProgressMs;
                            if (!artifactHttp.connected() ||
                                idleMs > Limits::FirmwareUpdate::Http::StreamReadTimeoutMs) {
                                retryableFailure = true;
                                LOGW("[OTA-HTTP] op=%lu target=%s attempt=%u stage=interrupted received=%lu/%lu skip=%lu idle_ms=%lu",
                                     (unsigned long)job.operationId, targetStr_(target),
                                     attempt, (unsigned long)received,
                                     (unsigned long)expectedSize, (unsigned long)replaySkip,
                                     (unsigned long)idleMs);
                                break;
                            }
                            if (millis() - lastLogMs >= 5000U) {
                                LOGW("[OTA] HTTP waiting target=%s received=%lu/%lu idle_ms=%lu attempt=%u",
                                     targetStr_(target), (unsigned long)received,
                                     (unsigned long)expectedSize, (unsigned long)idleMs, attempt);
                                lastLogMs = millis();
                            }
                            vTaskDelay(1);
                            continue;
                        }
                        size_t amount = available > sizeof(chunk) ? sizeof(chunk) : available;
                        if (replaySkip > 0U) {
                            if (amount > replaySkip) amount = replaySkip;
                        } else if (amount > expectedSize - received) {
                            amount = expectedSize - received;
                        }
                        const int rd = stream->readBytes(reinterpret_cast<char*>(chunk), amount);
                        if (rd <= 0) {
                            if (!artifactHttp.connected() ||
                                millis() - lastProgressMs > Limits::FirmwareUpdate::Http::StreamReadTimeoutMs) {
                                retryableFailure = true;
                                LOGW("[OTA-HTTP] op=%lu target=%s attempt=%u stage=read_stalled offset=%lu",
                                     (unsigned long)job.operationId, targetStr_(target),
                                     attempt, (unsigned long)received);
                                break;
                            }
                            vTaskDelay(1);
                            continue;
                        }
                        lastProgressMs = millis();
                        if (replaySkip > 0U) {
                            replaySkip -= (size_t)rd;
                            vTaskDelay(1);
                            continue;
                        }
                        const uint32_t writeStartUs = micros();
                        success = writeLocalImage_(transactionId, target, chunk, (size_t)rd,
                                                   received, errOut, errOutLen);
                        const uint32_t writeUs = (uint32_t)(micros() - writeStartUs);
                        if (writeUs > maxFlashWriteUs) maxFlashWriteUs = writeUs;
                        if (writeUs >= 20000U) ++slowFlashWrites;
                        if (!success) {
                            LOGE("[OTA-TRACE] op=%lu target=%s stage=flash_error offset=%lu write_us=%lu reason=%s",
                                 (unsigned long)job.operationId, targetStr_(target),
                                 (unsigned long)received, (unsigned long)writeUs,
                                 errOut ? errOut : "-");
                            otaMonitor_("flash_error");
                            break;
                        }
                        received += (size_t)rd;
                        if (received - lastLoggedBytes >= 256U * 1024U ||
                            received == expectedSize) {
                            RebootDiagnostics::noteOta(job.operationId,
                                imageIndex == 0U ? RebootDiagnostics::OtaStage::Spiffs
                                                 : RebootDiagnostics::OtaStage::Firmware,
                                (uint32_t)received, expectedSize);
                            LOGI("[OTA-TRACE] op=%lu target=%s stage=stream_progress received=%lu/%lu flash_max_us=%lu flash_slow20ms=%lu attempt=%u",
                                 (unsigned long)job.operationId, targetStr_(target),
                                 (unsigned long)received, (unsigned long)expectedSize,
                                 (unsigned long)maxFlashWriteUs, (unsigned long)slowFlashWrites,
                                 attempt);
                            otaMonitor_(imageIndex == 0U ? "spiffs_progress" : "firmware_progress");
                            lastLoggedBytes = received;
                            lastLogMs = lastProgressMs;
                        }
                        vTaskDelay(1);
                    }
                }
                artifactHttp.end();
            }
            LOGI("[OTA-TRACE] op=%lu target=%s stage=http_attempt_end attempt=%u success=%u received=%lu/%lu",
                 (unsigned long)job.operationId, targetStr_(target), attempt,
                 (success && !retryableFailure && received == expectedSize) ? 1U : 0U,
                 (unsigned long)received, (unsigned long)expectedSize);
            if (success && !retryableFailure && received == expectedSize) break;
            if (!success) break; // Flash or validation error: never retry.
            if (!retryableFailure) {
                success = writeSimpleError_(errOut, errOutLen, "streaming artifact incomplete");
                break;
            }
            if (attempt == OtaHttpRetryPolicy::kMaxAttempts) {
                LOGE("[OTA-HTTP] op=%lu target=%s stage=retries_exhausted attempts=%u received=%lu/%lu mode=stream",
                     (unsigned long)job.operationId, targetStr_(target),
                     OtaHttpRetryPolicy::kMaxAttempts,
                     (unsigned long)received, (unsigned long)expectedSize);
                success = writeSimpleError_(errOut, errOutLen,
                    "streaming artifact failed after 3 HTTP attempts");
                break;
            }
            LOGW("[OTA-HTTP] op=%lu target=%s stage=retry_scheduled next=%u/%u offset=%lu mode=stream",
                 (unsigned long)job.operationId, targetStr_(target),
                 attempt + 1U, OtaHttpRetryPolicy::kMaxAttempts,
                 (unsigned long)received);
            vTaskDelay(pdMS_TO_TICKS(OtaHttpRetryPolicy::kRetryDelayMs));
        }
        LOGI("[OTA-TRACE] op=%lu target=%s stage=http_end success=%u",
             (unsigned long)job.operationId, targetStr_(target),
             (success && received == expectedSize) ? 1U : 0U);
        if (success) {
            // The transport count and flash transaction count MUST agree
            // before an image can be finalized. A mismatched stage/counter
            // reveals unexpected concurrent mutation or an upload boundary
            // bug instead of producing a misleading SHA/short-image error.
            size_t flashed = 0U;
            unsigned stage = 0U;
            bool transactionMatches = false;
            {
                SemaphoreGuard imageGuard(localReleaseMutex_);
                if (imageGuard.acquired()) {
                    flashed = localRelease_.received;
                    stage = (unsigned)localRelease_.stage;
                    transactionMatches = localTransactionMatches_(transactionId);
                }
            }
            if (!transactionMatches || flashed != expectedSize) {
                LOGE("[OTA] stream/flash mismatch target=%s flashed=%lu expected=%lu stage=%u transaction=%u",
                     targetStr_(target), (unsigned long)flashed,
                     (unsigned long)expectedSize, stage, transactionMatches ? 1U : 0U);
                success = writeSimpleError_(errOut, errOutLen, "release transport/flash counter mismatch");
            } else {
                LOGI("[OTA] transport and flash counters match target=%s bytes=%lu",
                     targetStr_(target), (unsigned long)flashed);
                RebootDiagnostics::noteOta(job.operationId,
                    imageIndex == 0U ? RebootDiagnostics::OtaStage::VerifySpiffs
                                     : RebootDiagnostics::OtaStage::VerifyFirmware,
                    expectedSize, expectedSize);
                LOGI("[OTA-TRACE] op=%lu target=%s stage=image_verify_start",
                     (unsigned long)job.operationId, targetStr_(target));
                const uint8_t verifyProgress = imageIndex == 0U
                    ? (uint8_t)(((uint64_t)fsSize * 100U) / ((uint64_t)fsSize + fwSize))
                    : 99U;
                setStatus_(UpdateState::Flashing, FirmwareUpdateTarget::Waveshare,
                           verifyProgress,
                           imageIndex == 0U ? "verifying release filesystem"
                                            : "verifying release firmware",
                           job.operationId);
                setOtaStage_(imageIndex == 0U ? "spiffs_verify" : "firmware_verify", 0U);
                const uint32_t verifyStartedMs = millis();
                success = endLocalImage_(transactionId, target, errOut, errOutLen);
                if (success) setOtaStage_(imageIndex == 0U ? "spiffs_verify" : "firmware_verify", 100U);
                LOGI("[OTA-TRACE] op=%lu target=%s stage=image_verify_end ms=%lu success=%u",
                     (unsigned long)job.operationId, targetStr_(target),
                     (unsigned long)(millis() - verifyStartedMs), success ? 1U : 0U);
                otaMonitor_(imageIndex == 0U ? "spiffs_verify_end" : "firmware_verify_end");
                if (!success) {
                    LOGE("[OTA] image finalization failed target=%s reason=%s",
                         targetStr_(target), errOut ? errOut : "-");
                }
            }
        }
    }

    if (success) {
        RebootDiagnostics::noteOta(job.operationId, RebootDiagnostics::OtaStage::Commit);
        setStatus_(UpdateState::Flashing, FirmwareUpdateTarget::Waveshare, 99U,
                   "activating paired release", job.operationId);
        setOtaStage_("activate", 0U);
        LOGI("[OTA-TRACE] op=%lu stage=commit_start", (unsigned long)job.operationId);
        otaMonitor_("commit_start");
        success = commitLocalRelease_(transactionId, errOut, errOutLen);
        LOGI("[OTA-TRACE] op=%lu stage=commit_end success=%u",
             (unsigned long)job.operationId, success ? 1U : 0U);
        otaMonitor_("commit_end");
        if (success) {
            LOGI("[OTA] paired release verified and committed; one reboot scheduled");
        }
    }
    char failedStage[sizeof(status_.otaStage)]{};
    if (!success) {
        // Remember the phase before abortLocalRelease_ resets the UI state to
        // Idle. The updater's final runJob_ error would otherwise report
        // stage=idle, losing the exact phase of the first failed attempt.
        portENTER_CRITICAL(&lock_);
        snprintf(failedStage, sizeof(failedStage), "%s",
                 status_.otaStage[0] ? status_.otaStage : stateStr_(status_.state));
        portEXIT_CRITICAL(&lock_);
        LOGE("[OTA-TRACE] op=%lu stage=aborted reason=%s",
             (unsigned long)job.operationId, errOut ? errOut : "-");
        otaMonitor_("abort");
    }
    if (!success && transactionId != 0U) {
        // Keeps the old paired release active on any download/validation fault.
        char abortError[128]{};
        (void)abortLocalRelease_(transactionId, abortError, sizeof(abortError));
    }
    if (!success) {
        // At most one small NVS write, after the transaction was aborted;
        // retain this error even if the user immediately retries and succeeds.
        persistLastFailure_(FirmwareUpdateTarget::Waveshare,
                            job.operationId, failedStage, errOut);
    }
    portENTER_CRITICAL(&lock_);
    remoteReleaseWorker_ = nullptr;
    remoteReleaseOperationId_ = 0U;
    portEXIT_CRITICAL(&lock_);
    return success;
}

bool FirmwareUpdateModule::runNextionUpdate_(const UpdateJob& job, char* errOut, size_t errOutLen)
{
    if (nextionRxPin_ < 0 || nextionTxPin_ < 0) {
        writeSimpleError_(errOut, errOutLen, "nextion board pins not configured");
        return false;
    }

    if (!hmiSvc_ && services_) {
        hmiSvc_ = services_->get<HmiService>(ServiceId::Hmi);
    }
    HmiDisplayIdentity identity{};
    // A display in bootloader or recovery mode may not answer the identity
    // query. The artifact was already selected from the validated manifest;
    // when an identity is available, it must still match that selection.
    const bool displayDetected =
        hmiSvc_ && hmiSvc_->getLocalDisplayIdentity &&
        hmiSvc_->getLocalDisplayIdentity(hmiSvc_->ctx, &identity);
    if (displayDetected && !isNextionDisplayCompatible(identity, job.nextionCompatibility)) {
        writeSimpleError_(errOut, errOutLen, "nextion display identity validation failed");
        return false;
    }

    setStatus_(UpdateState::Downloading,
               FirmwareUpdateTarget::Nextion,
               0,
               "downloading",
               job.operationId);

    if (flowIoEnablePin_ >= 0) {
        pinMode(flowIoEnablePin_, OUTPUT);
        digitalWrite(flowIoEnablePin_, LOW);
    }

    if (nextionRebootPin_ >= 0) {
        pinMode(nextionRebootPin_, OUTPUT);
        digitalWrite(nextionRebootPin_, HIGH);
    }

    HTTPClient http;
    configureDownloadHttp_(http);
    if (!http.begin(job.url)) {
        writeHttpBeginFailedError_("fichier de mise a jour", job.url, errOut, errOutLen);
        if (flowIoEnablePin_ >= 0) {
            digitalWrite(flowIoEnablePin_, HIGH);
            pinMode(flowIoEnablePin_, INPUT);
        }
        return false;
    }

    const int code = http.GET();
    const int32_t contentLength = http.getSize();
    if (code != HTTP_CODE_OK) {
        writeHttpCodeFailedError_("fichier de mise a jour", job.url, http, code, errOut, errOutLen);
        http.end();
        if (flowIoEnablePin_ >= 0) {
            digitalWrite(flowIoEnablePin_, HIGH);
            pinMode(flowIoEnablePin_, INPUT);
        }
        return false;
    }
    if (contentLength <= 0) {
        writeSimpleError_(errOut, errOutLen, "invalid content-length");
        http.end();
        if (flowIoEnablePin_ >= 0) {
            digitalWrite(flowIoEnablePin_, HIGH);
            pinMode(flowIoEnablePin_, INPUT);
        }
        return false;
    }
    if (job.expectedSize == 0U || (uint32_t)contentLength != job.expectedSize) {
        writeSimpleError_(errOut, errOutLen, "nextion artifact size differs from manifest");
        http.end();
        if (flowIoEnablePin_ >= 0) {
            digitalWrite(flowIoEnablePin_, HIGH);
            pinMode(flowIoEnablePin_, INPUT);
        }
        return false;
    }

    setStatus_(UpdateState::Flashing,
               FirmwareUpdateTarget::Nextion,
               0,
               "flashing",
               job.operationId);
    if (!hmiSvc_->setLocalDisplayUpdateMode ||
        !hmiSvc_->setLocalDisplayUpdateMode(hmiSvc_->ctx, true, 1000U)) {
        writeSimpleError_(errOut, errOutLen, "failed to suspend nextion hmi driver");
        http.end();
        if (flowIoEnablePin_ >= 0) {
            digitalWrite(flowIoEnablePin_, HIGH);
            pinMode(flowIoEnablePin_, INPUT);
        }
        return false;
    }
    portENTER_CRITICAL(&lock_);
    activeTotalBytes_ = (uint32_t)contentLength;
    activeSentBytes_ = 0;
    portEXIT_CRITICAL(&lock_);

    bool ok = false;
    ESPNexUpload nextion(nextionUploadBaud_, nextionRxPin_, nextionTxPin_);
    if (!nextion.prepareUpload((uint32_t)contentLength)) {
        writeSimpleError_(errOut, errOutLen, nextion.statusMessage.c_str());
    } else {
        auto* stream = http.getStreamPtr();
        uint8_t buffer[2048] = {0};
        uint32_t remaining = (uint32_t)contentLength;
        uint32_t lastReadMs = millis();
        ok = true;

        while (remaining > 0U) {
            const size_t available = stream ? stream->available() : 0U;
            if (available == 0U) {
                if (!http.connected()) {
                    writeSimpleError_(errOut, errOutLen, "nextion download interrupted");
                    ok = false;
                    break;
                }
                if ((millis() - lastReadMs) > Limits::FirmwareUpdate::Http::StreamReadTimeoutMs) {
                    writeSimpleError_(errOut, errOutLen, "nextion stream timeout");
                    ok = false;
                    break;
                }
                delay(1);
                continue;
            }

            size_t toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
            if (toRead > remaining) toRead = remaining;
            const int read = stream->readBytes(reinterpret_cast<char*>(buffer), toRead);
            if (read <= 0) {
                delay(1);
                continue;
            }
            lastReadMs = millis();

            if (!nextion.upload(buffer, (size_t)read)) {
                writeSimpleError_(errOut, errOutLen, nextion.statusMessage.c_str());
                ok = false;
                break;
            }
            remaining -= (uint32_t)read;
            onProgressChunk_((uint32_t)read);
        }
    }
    nextion.end();

    pinMode(nextionRxPin_, INPUT);
    pinMode(nextionTxPin_, INPUT);

    http.end();
    if (flowIoEnablePin_ >= 0) {
        digitalWrite(flowIoEnablePin_, HIGH);
        pinMode(flowIoEnablePin_, INPUT);
    }

    if (!ok) {
        (void)hmiSvc_->setLocalDisplayUpdateMode(hmiSvc_->ctx, false, 1000U);
        return false;
    }

    if (!persistReceipt_(job.target,
                         job.operationId,
                         FirmwareUpdateReceiptState::RebootPending)) {
        (void)hmiSvc_->setLocalDisplayUpdateMode(hmiSvc_->ctx, false, 1000U);
        writeSimpleError_(errOut, errOutLen, "failed to persist update completion");
        return false;
    }
    setStatus_(UpdateState::Rebooting,
               FirmwareUpdateTarget::Nextion,
               100,
               "rebooting after nextion update",
               job.operationId);
    delay(1800);
    if (!saveCountersBeforeUpdate_(errOut, errOutLen)) return false;
    RebootDiagnostics::markPlanned(RebootDiagnostics::Origin::OtaNextion, job.operationId);
    ESP.restart();
    return true;
}

bool FirmwareUpdateModule::runNextionReboot_(char* errOut, size_t errOutLen)
{
    if (nextionRebootPin_ < 0) {
        writeSimpleError_(errOut, errOutLen, "nextion reboot pin not configured");
        return false;
    }

    pinMode(nextionRebootPin_, OUTPUT);
    digitalWrite(nextionRebootPin_, HIGH);
    vTaskDelay(pdMS_TO_TICKS(500));
    digitalWrite(nextionRebootPin_, LOW);
    vTaskDelay(pdMS_TO_TICKS(500));
    digitalWrite(nextionRebootPin_, HIGH);
    vTaskDelay(pdMS_TO_TICKS(500));
    digitalWrite(nextionRebootPin_, LOW);

    LOGI("Nextion reboot pulse sequence completed on pin=%d", (int)nextionRebootPin_);
    return true;
}

bool FirmwareUpdateModule::runSpiffsUpdate_(const UpdateJob& job, char* errOut, size_t errOutLen)
{
    // Legacy SPIFFS button is a migration trigger ONLY. It must never
    // erase or write the mounted filesystem. The paired updater below writes
    // firmware + filesystem to inactive A/B slots, validates both, and
    // changes the boot slot after commit.
    if (!ReleaseStorage::legacyPairedMigrationNeeded()) {
        return writeSimpleError_(errOut, errOutLen,
                                 "standalone SPIFFS OTA forbidden; use paired FlowIO update");
    }
    LOGI("[OTA] legacy SPIFFS button: starting verified paired A/B migration");
    return runWaveshareUpdate_(job, errOut, errOutLen);
}
bool FirmwareUpdateModule::localTransactionMatches_(uint32_t transactionId) const
{
    return localRelease_.active && transactionId != 0U && localRelease_.id == transactionId;
}

void FirmwareUpdateModule::resetLocalRelease_()
{
    if (localRelease_.shaActive) {
        mbedtls_sha256_free(&localRelease_.shaContext);
    }
    if (localRelease_.otaHandle != 0) {
        (void)esp_ota_abort(localRelease_.otaHandle);
    }
    localRelease_ = LocalReleaseTransaction{};
    portENTER_CRITICAL(&lock_);
    localReleaseActive_ = false;
    activeTotalBytes_ = 0U;
    activeSentBytes_ = 0U;
    portEXIT_CRITICAL(&lock_);
    setHmiOtaCondition_(false);
}

void FirmwareUpdateModule::failLocalRelease_(const char* reason)
{
    if (localRelease_.stage == LocalReleaseStage::Error) return;
    if (localRelease_.shaActive) {
        mbedtls_sha256_free(&localRelease_.shaContext);
        localRelease_.shaActive = false;
    }
    if (localRelease_.otaHandle != 0) {
        (void)esp_ota_abort(localRelease_.otaHandle);
        localRelease_.otaHandle = 0;
    }
    const FirmwareUpdateTarget target =
        localRelease_.stage == LocalReleaseStage::WritingFirmware ||
                localRelease_.stage == LocalReleaseStage::FirmwareVerified ||
                localRelease_.stage == LocalReleaseStage::ReadyToBoot
            ? FirmwareUpdateTarget::Waveshare
            : FirmwareUpdateTarget::Spiffs;
    snprintf(localRelease_.failureReason,
             sizeof(localRelease_.failureReason),
             "%s",
             reason ? reason : "local release failed");
    localRelease_.stage = LocalReleaseStage::Error;
    setError_(target, localRelease_.failureReason, localRelease_.operationId);
}

bool FirmwareUpdateModule::beginLocalRelease_(const char* manifestJson,
                                               size_t manifestLen,
                                               uint32_t* transactionIdOut,
                                               char* errOut,
                                               size_t errOutLen)
{
    SemaphoreGuard transactionGuard(localReleaseMutex_);
    if (!transactionGuard.acquired()) {
        return writeSimpleError_(errOut, errOutLen, "upgrade synchronization unavailable");
    }
    if (transactionIdOut) *transactionIdOut = 0U;
    if (!manifestJson || manifestLen == 0U || manifestLen > 1024U) {
        return writeSimpleError_(errOut, errOutLen, "invalid release manifest");
    }
    // This is called from the remote OTA worker, which is already several
    // stack frames deep. Keep the JSON tree on the heap, and NEVER mutate the
    // manifest buffer that has already been parsed by the caller.
    // This second manifest parse can overlap the caller's JSON pool.
    // Place both pools explicitly in PSRAM to protect lwIP/OTA internal RAM.
    SpiRamJsonDocument doc(2048U);
    if (doc.capacity() == 0U) {
        return writeSimpleError_(errOut, errOutLen, "release manifest json psram unavailable");
    }
    otaMonitor_("release_manifest_json_psram");
    const auto jsonError = deserializeJson(doc, static_cast<const char*>(manifestJson), manifestLen);
    if (jsonError || !doc.is<JsonObjectConst>()) {
        LOGE("[OTA] paired release second parse failed: %s", jsonError.c_str());
        return writeSimpleError_(errOut, errOutLen, "invalid release manifest json");
    }
    const uint32_t format = doc["format"] | 0U;
    const char* product = doc["product"] | "";
    const char* hardware = doc["hardware"] | "";
    const char* version = doc["version"] | "";
    const JsonObjectConst firmware = doc["firmware"].as<JsonObjectConst>();
    const JsonObjectConst filesystem = doc["filesystem"].as<JsonObjectConst>();
    if (format != 1U || strcmp(product, "Flow.IO") != 0 || !hardwareName_ ||
        strcmp(hardware, hardwareName_) != 0 || version[0] == '\0' ||
        firmware.isNull() || filesystem.isNull()) {
        return writeSimpleError_(errOut, errOutLen, "incompatible release manifest");
    }

    LocalReleaseTransaction candidate{};
    if (strlen(version) >= sizeof(candidate.version)) {
        return writeSimpleError_(errOut, errOutLen, "release version is too long");
    }
    candidate.targetSlot = ReleaseStorage::inactiveSlot();
    candidate.filesystem.size = filesystem["size"] | 0U;
    candidate.firmware.size = firmware["size"] | 0U;
    const char* firmwareFile = firmware["file"] | "";
    const char* filesystemFile = filesystem["file"] | "";
    const bool fromPairedRemoteWorker = remoteReleaseWorker_ != nullptr &&
                                        remoteReleaseWorker_ == xTaskGetCurrentTaskHandle();
    const bool filenamesValid = fromPairedRemoteWorker
        ? (isSimpleArtifactFilename_(firmwareFile) &&
           isSimpleArtifactFilename_(filesystemFile) &&
           strcmp(firmwareFile, filesystemFile) != 0)
        : (strcmp(firmwareFile, "firmware.bin") == 0 &&
           strcmp(filesystemFile, "spiffs.bin") == 0);
    if (!filenamesValid ||
        candidate.filesystem.size == 0U || candidate.firmware.size == 0U ||
        !parseSha256_(filesystem["sha256"] | "", candidate.filesystem.sha256) ||
        !parseSha256_(firmware["sha256"] | "", candidate.firmware.sha256)) {
        return writeSimpleError_(errOut, errOutLen, "incomplete release manifest");
    }

    const esp_partition_t* fsPartition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        ReleaseStorage::filesystemLabel(candidate.targetSlot));
    const esp_partition_subtype_t appSubtype = candidate.targetSlot == ReleaseSlot::A
                                                   ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                                   : ESP_PARTITION_SUBTYPE_APP_OTA_1;
    const esp_partition_t* appPartition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, appSubtype, ReleaseStorage::applicationLabel(candidate.targetSlot));
    if (!fsPartition || !appPartition || candidate.filesystem.size != fsPartition->size ||
        candidate.firmware.size > appPartition->size) {
        return writeSimpleError_(errOut, errOutLen, "release image exceeds target partition");
    }

    candidate.active = true;
    candidate.id = esp_random();
    if (candidate.id == 0U) candidate.id = 1U;
    candidate.stage = LocalReleaseStage::Prepared;
    candidate.lastActivityMs = millis();
    snprintf(candidate.version, sizeof(candidate.version), "%s", version);

    portENTER_CRITICAL(&lock_);
    // Internal remote worker is already marked busy_ by loop(); external API
    // callers cannot bypass the busy gate because their FreeRTOS task differs.
    const bool internalRemoteWorker = remoteReleaseWorker_ != nullptr &&
                                      remoteReleaseWorker_ == xTaskGetCurrentTaskHandle();
    if ((busy_ && !internalRemoteWorker) || localReleaseActive_ || queuedJob_.pending ||
        nextionRebootQueued_ || updateStartPending_ || !manifestMetadata_ ||
        manifestCheckIsActive_(manifestMetadata_->check.state) || manifestCopyReaders_ > 0U) {
        portEXIT_CRITICAL(&lock_);
        return writeSimpleError_(errOut, errOutLen, "updater busy");
    }
    localReleaseActive_ = true;
    if (internalRemoteWorker) {
        // Preserve the operation ID returned to the Web client when queued.
        candidate.operationId = remoteReleaseOperationId_;
    } else {
        candidate.operationId = nextOperationId_++;
        if (nextOperationId_ == 0U) nextOperationId_ = 1U;
    }
    portEXIT_CRITICAL(&lock_);

    localRelease_ = candidate;
    LOGI("[OTA] paired transaction checkpoint begin operation_id=%lu stack_free_bytes=%u",
         (unsigned long)candidate.operationId,
         (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    if (!saveCountersBeforeUpdate_(errOut, errOutLen)) {
        LOGE("[OTA] paired transaction checkpoint failed: %s", errOut ? errOut : "-");
        resetLocalRelease_();
        return false;
    }
    LOGI("[OTA] paired transaction checkpoint OK; persisting prepared receipt");
    if (!persistReceipt_(FirmwareUpdateTarget::Waveshare,
                         candidate.operationId,
                         FirmwareUpdateReceiptState::Running)) {
        resetLocalRelease_();
        return writeSimpleError_(errOut, errOutLen, "failed to persist local update operation");
    }
    if (transactionIdOut) *transactionIdOut = candidate.id;
    setStatus_(UpdateState::Queued,
               internalRemoteWorker ? FirmwareUpdateTarget::Waveshare : FirmwareUpdateTarget::Spiffs,
               0U,
               internalRemoteWorker ? "remote paired release prepared" : "local release prepared",
               candidate.operationId);
    LOGI("[UPGRADE] begin release=%s target=%s",
         candidate.version,
         candidate.targetSlot == ReleaseSlot::A ? "A" : "B");
    return true;
}

bool FirmwareUpdateModule::beginLocalImage_(uint32_t transactionId,
                                             FirmwareUpdateTarget target,
                                             size_t totalSize,
                                             char* errOut,
                                             size_t errOutLen)
{
    SemaphoreGuard transactionGuard(localReleaseMutex_);
    if (!transactionGuard.acquired()) {
        return writeSimpleError_(errOut, errOutLen, "upgrade synchronization unavailable");
    }
    // A remote paired transaction owns its upload/validation methods. Do not
    // permit an asynchronous Web upload callback to finish or alter the same
    // transaction while the OTA worker is still receiving its image.
    if (remoteReleaseWorker_ != nullptr &&
        remoteReleaseWorker_ != xTaskGetCurrentTaskHandle()) {
        return writeSimpleError_(errOut, errOutLen, "remote paired release owns image transaction");
    }
    if (!isLocalReleaseImageTarget_(target)) {
        return writeSimpleError_(errOut, errOutLen, "unsupported release image target");
    }
    if (!localTransactionMatches_(transactionId)) {
        return writeSimpleError_(errOut, errOutLen, "unknown release transaction");
    }
    if (localRelease_.stage == LocalReleaseStage::Error) {
        return writeSimpleError_(errOut,
                                 errOutLen,
                                 localRelease_.failureReason[0]
                                     ? localRelease_.failureReason
                                     : "release transaction failed");
    }
    const bool filesystemTarget = target == FirmwareUpdateTarget::Spiffs;
    const LocalImageManifest& image = filesystemTarget ? localRelease_.filesystem : localRelease_.firmware;
    const LocalReleaseStage expectedStage = filesystemTarget
                                                ? LocalReleaseStage::Prepared
                                                : LocalReleaseStage::FilesystemVerified;
    if (localRelease_.stage != expectedStage || totalSize != image.size) {
        return writeSimpleError_(errOut, errOutLen, "unexpected release image");
    }

    const esp_partition_type_t type = filesystemTarget ? ESP_PARTITION_TYPE_DATA : ESP_PARTITION_TYPE_APP;
    const esp_partition_subtype_t subtype = filesystemTarget
                                                ? ESP_PARTITION_SUBTYPE_ANY
                                                : (localRelease_.targetSlot == ReleaseSlot::A
                                                       ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                                       : ESP_PARTITION_SUBTYPE_APP_OTA_1);
    const char* label = filesystemTarget
                            ? ReleaseStorage::filesystemLabel(localRelease_.targetSlot)
                            : ReleaseStorage::applicationLabel(localRelease_.targetSlot);
    localRelease_.writePartition = esp_partition_find_first(type, subtype, label);
    if (!localRelease_.writePartition || totalSize > localRelease_.writePartition->size) {
        failLocalRelease_("target partition unavailable");
        return writeSimpleError_(errOut, errOutLen, "target partition unavailable");
    }

    // A/B release uploads must never erase the active filesystem or firmware.
    const esp_partition_t* active = filesystemTarget
        ? esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                   ReleaseStorage::filesystemLabel(ReleaseStorage::runningSlot()))
        : esp_ota_get_running_partition();
    const esp_partition_t* destination = localRelease_.writePartition;
    if (!active || (active->address < destination->address + destination->size &&
                    destination->address < active->address + active->size)) {
        failLocalRelease_("release target overlaps active partition");
        return writeSimpleError_(errOut, errOutLen, "release target overlaps active partition");
    }

    esp_err_t flashError = ESP_OK;
    if (filesystemTarget) {
        // Erase the inactive SPIFFS in bounded chunks. This path serves both
        // local and remote paired releases, so never monopolize the Wi-Fi CPU.
        constexpr size_t kEraseBytes = 64U * 1024U;
        const size_t partitionSize = localRelease_.writePartition->size;
        if (partitionSize % kEraseBytes != 0U) {
            failLocalRelease_("inactive filesystem erase alignment invalid");
            return writeSimpleError_(errOut, errOutLen, "inactive filesystem erase alignment invalid");
        }
        for (size_t offset = 0U; offset < partitionSize; offset += kEraseBytes) {
            flashError = esp_partition_erase_range(localRelease_.writePartition, offset, kEraseBytes);
            if (flashError != ESP_OK) break;
            vTaskDelay(1);
        }
    } else {
        flashError = esp_ota_begin(localRelease_.writePartition, totalSize, &localRelease_.otaHandle);
    }
    if (flashError != ESP_OK) {
        failLocalRelease_("failed to initialize target partition");
        return writeSimpleError_(errOut, errOutLen, "failed to initialize target partition");
    }

    mbedtls_sha256_init(&localRelease_.shaContext);
    if (mbedtls_sha256_starts(&localRelease_.shaContext, 0) != 0) {
        failLocalRelease_("sha256 initialization failed");
        return writeSimpleError_(errOut, errOutLen, "sha256 initialization failed");
    }
    localRelease_.shaActive = true;
    localRelease_.received = 0U;
    localRelease_.lastActivityMs = millis();
    localRelease_.stage = filesystemTarget ? LocalReleaseStage::WritingFilesystem
                                           : LocalReleaseStage::WritingFirmware;
    const bool remotePaired = remoteReleaseWorker_ != nullptr &&
                              remoteReleaseWorker_ == xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&lock_);
    if (remotePaired) {
        activeTotalBytes_ = localRelease_.filesystem.size + localRelease_.firmware.size;
        if (filesystemTarget) activeSentBytes_ = 0U;
    } else {
        activeTotalBytes_ = (uint32_t)totalSize;
        activeSentBytes_ = 0U;
    }
    const uint8_t currentProgress = activeTotalBytes_
        ? (uint8_t)(((uint64_t)activeSentBytes_ * 100U) / activeTotalBytes_) : 0U;
    portEXIT_CRITICAL(&lock_);
    setStatus_(UpdateState::Flashing,
               remotePaired ? FirmwareUpdateTarget::Waveshare : target,
               currentProgress,
               filesystemTarget ? "writing release filesystem" : "writing release firmware",
               localRelease_.operationId);
    LOGI("[UPGRADE] %s target=%s size=%u",
         filesystemTarget ? "filesystem" : "firmware",
         label,
         (unsigned)totalSize);
    return true;
}

bool FirmwareUpdateModule::writeLocalImage_(uint32_t transactionId,
                                             FirmwareUpdateTarget target,
                                             const uint8_t* data,
                                             size_t len,
                                             size_t offset,
                                             char* errOut,
                                             size_t errOutLen)
{
    SemaphoreGuard transactionGuard(localReleaseMutex_);
    if (!transactionGuard.acquired()) {
        return writeSimpleError_(errOut, errOutLen, "upgrade synchronization unavailable");
    }
    // A remote paired transaction owns its upload/validation methods. Do not
    // permit an asynchronous Web upload callback to finish or alter the same
    // transaction while the OTA worker is still receiving its image.
    if (remoteReleaseWorker_ != nullptr &&
        remoteReleaseWorker_ != xTaskGetCurrentTaskHandle()) {
        return writeSimpleError_(errOut, errOutLen, "remote paired release owns image transaction");
    }
    if (!isLocalReleaseImageTarget_(target)) {
        return writeSimpleError_(errOut, errOutLen, "unsupported release image target");
    }
    if (!localTransactionMatches_(transactionId) || !data || len == 0U) {
        return writeSimpleError_(errOut, errOutLen, "invalid release image chunk");
    }
    const bool filesystemTarget = target == FirmwareUpdateTarget::Spiffs;
    const LocalReleaseStage expectedStage = filesystemTarget
                                                ? LocalReleaseStage::WritingFilesystem
                                                : LocalReleaseStage::WritingFirmware;
    const size_t expectedSize = filesystemTarget ? localRelease_.filesystem.size
                                                 : localRelease_.firmware.size;
    if (localRelease_.stage != expectedStage || offset != localRelease_.received ||
        len > expectedSize - localRelease_.received) {
        failLocalRelease_("out-of-order release image chunk");
        return writeSimpleError_(errOut, errOutLen, "out-of-order release image chunk");
    }

    if (offset == 0U) {
        LOGI("[UPGRADE] %s stream first_chunk=%u expected=%u",
             filesystemTarget ? "filesystem" : "firmware",
             (unsigned)len,
             (unsigned)expectedSize);
    }

    const esp_err_t writeError = filesystemTarget
                                     ? esp_partition_write(localRelease_.writePartition, offset, data, len)
                                     : esp_ota_write(localRelease_.otaHandle, data, len);
    if (writeError != ESP_OK || mbedtls_sha256_update(&localRelease_.shaContext, data, len) != 0) {
        failLocalRelease_("release image write failed");
        return writeSimpleError_(errOut, errOutLen, "release image write failed");
    }
    localRelease_.received += len;
    localRelease_.lastActivityMs = millis();
    onProgressChunk_((uint32_t)len);
    if (remoteReleaseWorker_ != nullptr &&
        remoteReleaseWorker_ == xTaskGetCurrentTaskHandle()) {
        // Image-local progress; preserve PSRAM write vs HTTP streaming mode.
        setOtaStage_(nullptr, (uint8_t)(((uint64_t)localRelease_.received * 100U) / expectedSize));
    }
    if (localRelease_.received == expectedSize) {
        LOGI("[UPGRADE] %s stream complete received=%u",
             filesystemTarget ? "filesystem" : "firmware",
             (unsigned)localRelease_.received);
    }
    return true;
}

bool FirmwareUpdateModule::endLocalImage_(uint32_t transactionId,
                                           FirmwareUpdateTarget target,
                                           char* errOut,
                                           size_t errOutLen)
{
    SemaphoreGuard transactionGuard(localReleaseMutex_);
    if (!transactionGuard.acquired()) {
        return writeSimpleError_(errOut, errOutLen, "upgrade synchronization unavailable");
    }
    // A remote paired transaction owns its upload/validation methods. Do not
    // permit an asynchronous Web upload callback to finish or alter the same
    // transaction while the OTA worker is still receiving its image.
    if (remoteReleaseWorker_ != nullptr &&
        remoteReleaseWorker_ != xTaskGetCurrentTaskHandle()) {
        return writeSimpleError_(errOut, errOutLen, "remote paired release owns image transaction");
    }
    if (!isLocalReleaseImageTarget_(target)) {
        return writeSimpleError_(errOut, errOutLen, "unsupported release image target");
    }
    if (!localTransactionMatches_(transactionId)) {
        return writeSimpleError_(errOut, errOutLen, "unknown release transaction");
    }
    if (localRelease_.stage == LocalReleaseStage::Error) {
        return writeSimpleError_(errOut,
                                 errOutLen,
                                 localRelease_.failureReason[0]
                                     ? localRelease_.failureReason
                                     : "release transaction failed");
    }
    const bool filesystemTarget = target == FirmwareUpdateTarget::Spiffs;
    const LocalReleaseStage expectedStage = filesystemTarget
                                                ? LocalReleaseStage::WritingFilesystem
                                                : LocalReleaseStage::WritingFirmware;
    const LocalImageManifest& image = filesystemTarget ? localRelease_.filesystem : localRelease_.firmware;
    if (localRelease_.stage != expectedStage || localRelease_.received != image.size ||
        !localRelease_.shaActive) {
        char reason[120] = {0};
        snprintf(reason,
                 sizeof(reason),
                 "incomplete release image received=%u expected=%u stage=%u sha=%u",
                 (unsigned)localRelease_.received,
                 (unsigned)image.size,
                 (unsigned)localRelease_.stage,
                 localRelease_.shaActive ? 1U : 0U);
        LOGE("[UPGRADE] %s", reason);
        failLocalRelease_(reason);
        return writeSimpleError_(errOut, errOutLen, reason);
    }

    uint8_t digest[32]{};
    const int shaError = mbedtls_sha256_finish(&localRelease_.shaContext, digest);
    mbedtls_sha256_free(&localRelease_.shaContext);
    localRelease_.shaActive = false;
    if (shaError != 0 || !sha256Matches_(digest, image.sha256)) {
        failLocalRelease_("release image sha256 mismatch");
        return writeSimpleError_(errOut, errOutLen, "release image sha256 mismatch");
    }

    if (filesystemTarget) {
        if (!candidateFilesystemIsValid_(ReleaseStorage::filesystemLabel(localRelease_.targetSlot),
                                         localRelease_.version,
                                         hardwareName_)) {
            LOGE("[UPGRADE] candidate filesystem validation failed label=%s release=%s hardware=%s",
                 ReleaseStorage::filesystemLabel(localRelease_.targetSlot),
                 localRelease_.version,
                 hardwareName_ ? hardwareName_ : "-");
            failLocalRelease_("candidate filesystem validation failed");
            return writeSimpleError_(errOut, errOutLen, "candidate filesystem validation failed");
        }
        localRelease_.stage = LocalReleaseStage::FilesystemVerified;
        LOGI("[UPGRADE] filesystem verified sha256=OK");
    } else {
        if (esp_ota_end(localRelease_.otaHandle) != ESP_OK) {
            localRelease_.otaHandle = 0;
            failLocalRelease_("candidate firmware validation failed");
            return writeSimpleError_(errOut, errOutLen, "candidate firmware validation failed");
        }
        localRelease_.otaHandle = 0;
        localRelease_.stage = LocalReleaseStage::FirmwareVerified;
        LOGI("[UPGRADE] firmware verified sha256=OK");
    }
    localRelease_.writePartition = nullptr;
    localRelease_.received = 0U;
    localRelease_.lastActivityMs = millis();
    const bool remotePaired = remoteReleaseWorker_ != nullptr &&
                              remoteReleaseWorker_ == xTaskGetCurrentTaskHandle();
    if (!remotePaired) {
        portENTER_CRITICAL(&lock_);
        activeTotalBytes_ = 0U;
        activeSentBytes_ = 0U;
        portEXIT_CRITICAL(&lock_);
    }
    if (remotePaired) {
        // Never report Done between the two images; the browser must wait
        // for the verified joint commit and the single reboot.
        setStatus_(UpdateState::Flashing, FirmwareUpdateTarget::Waveshare,
                   filesystemTarget ? (uint8_t)(((uint64_t)localRelease_.filesystem.size * 100U) /
                                         (localRelease_.filesystem.size + localRelease_.firmware.size))
                                    : 99U,
                   filesystemTarget ? "filesystem SHA-256 OK; firmware next"
                                    : "firmware SHA-256 OK; committing release",
                   localRelease_.operationId);
    } else {
        // ZIP upload: an image has been verified, NOT the complete release.
        // Keep the paired OTA in Flashing until commitLocalRelease_ has
        // successfully verified both images and selected the inactive app.
        // Reporting Done here could trigger a premature browser reconnect
        // or give the operator a false success after SPIFFS alone.
        const uint8_t pairedProgress = filesystemTarget
            ? (uint8_t)(((uint64_t)localRelease_.filesystem.size * 100U) /
                        ((uint64_t)localRelease_.filesystem.size + localRelease_.firmware.size))
            : 99U;
        setStatus_(UpdateState::Flashing,
                   FirmwareUpdateTarget::Waveshare,
                   pairedProgress,
                   filesystemTarget ? "filesystem SHA-256 OK; firmware next"
                                    : "firmware SHA-256 OK; committing release",
                   localRelease_.operationId);
    }
    return true;
}

bool FirmwareUpdateModule::commitLocalRelease_(uint32_t transactionId,
                                                char* errOut,
                                                size_t errOutLen)
{
    SemaphoreGuard transactionGuard(localReleaseMutex_);
    if (!transactionGuard.acquired()) {
        return writeSimpleError_(errOut, errOutLen, "upgrade synchronization unavailable");
    }
    if (!localTransactionMatches_(transactionId) ||
        localRelease_.stage != LocalReleaseStage::FirmwareVerified) {
        LOGW("[UPGRADE] commit rejected stage=%u transaction_match=%u",
             (unsigned)localRelease_.stage,
             localTransactionMatches_(transactionId) ? 1U : 0U);
        return writeSimpleError_(errOut, errOutLen, "release is not ready to commit");
    }
    const esp_partition_subtype_t subtype = localRelease_.targetSlot == ReleaseSlot::A
                                                ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                                : ESP_PARTITION_SUBTYPE_APP_OTA_1;
    const esp_partition_t* target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, subtype, ReleaseStorage::applicationLabel(localRelease_.targetSlot));
    if (!target) {
        LOGE("[UPGRADE] boot partition %s not found",
             ReleaseStorage::applicationLabel(localRelease_.targetSlot));
        failLocalRelease_("failed to select release boot partition");
        return writeSimpleError_(errOut, errOutLen, "failed to select release boot partition");
    }
    // A verified release may be committed only after a confirmed checkpoint.
    if (!saveCountersBeforeUpdate_(errOut, errOutLen)) return false;
    if (!persistReceipt_(FirmwareUpdateTarget::Waveshare,
                         localRelease_.operationId,
                         FirmwareUpdateReceiptState::RebootPending)) {
        failLocalRelease_("failed to persist update completion");
        return writeSimpleError_(errOut, errOutLen, "failed to persist update completion");
    }
    const esp_err_t bootError = esp_ota_set_boot_partition(target);
    if (bootError != ESP_OK) {
        (void)persistReceipt_(FirmwareUpdateTarget::Waveshare,
                              localRelease_.operationId,
                              FirmwareUpdateReceiptState::Failed);
        LOGE("[UPGRADE] esp_ota_set_boot_partition(%s) failed: %s",
             ReleaseStorage::applicationLabel(localRelease_.targetSlot),
             esp_err_to_name(bootError));
        failLocalRelease_("failed to select release boot partition");
        return writeSimpleError_(errOut, errOutLen, "failed to select release boot partition");
    }
    localRelease_.stage = LocalReleaseStage::ReadyToBoot;
    localRelease_.rebootPending = true;
    localRelease_.rebootAtMs = millis() + 1800U;
    setStatus_(UpdateState::Rebooting,
               FirmwareUpdateTarget::Waveshare,
               100U,
               "release committed; rebooting",
               localRelease_.operationId);
    LOGI("[UPGRADE] release verified; boot partition -> %s",
         ReleaseStorage::applicationLabel(localRelease_.targetSlot));
    return true;
}

bool FirmwareUpdateModule::prepareLocalReleaseReboot_()
{
    SemaphoreGuard transactionGuard(localReleaseMutex_);
    if (!transactionGuard.acquired() || !localRelease_.rebootPending) return false;
    char error[96]{};
    if (saveCountersBeforeUpdate_(error, sizeof(error))) return true;
    localRelease_.rebootPending = false;
    // Commit already selected the new slot. Restore the running application so
    // a later unrelated reset does not silently complete this failed upgrade.
    const auto* running = esp_ota_get_running_partition();
    const bool restored = running && esp_ota_set_boot_partition(running) == ESP_OK;
    (void)persistReceipt_(FirmwareUpdateTarget::Waveshare, localRelease_.operationId,
                          FirmwareUpdateReceiptState::Failed);
    failLocalRelease_(restored ? "counter checkpoint failed; reboot cancelled"
                              : "counter checkpoint failed; reboot cancelled; boot partition restore failed");
    return false;
}

bool FirmwareUpdateModule::abortLocalRelease_(uint32_t transactionId,
                                               char* errOut,
                                               size_t errOutLen)
{
    SemaphoreGuard transactionGuard(localReleaseMutex_);
    if (!transactionGuard.acquired()) {
        return writeSimpleError_(errOut, errOutLen, "upgrade synchronization unavailable");
    }
    if (!localTransactionMatches_(transactionId)) {
        return writeSimpleError_(errOut, errOutLen, "unknown release transaction");
    }
    if (localRelease_.rebootPending) {
        return writeSimpleError_(errOut, errOutLen, "committed release cannot be aborted");
    }
    const uint32_t operationId = localRelease_.operationId;
    (void)persistReceipt_(FirmwareUpdateTarget::Waveshare,
                          operationId,
                          FirmwareUpdateReceiptState::Failed);
    resetLocalRelease_();
    setStatus_(UpdateState::Idle,
               FirmwareUpdateTarget::Waveshare,
               0U,
               "local release aborted",
               operationId);
    return true;
}

bool FirmwareUpdateModule::runJob_(const UpdateJob& job)
{
    if (!netAccessSvc_ && services_) {
        netAccessSvc_ = services_->get<NetworkAccessService>(ServiceId::NetworkAccess);
    }
    bool netReady = false;
    if (netAccessSvc_ && netAccessSvc_->isWebReachable) {
        netReady = netAccessSvc_->isWebReachable(netAccessSvc_->ctx);
    } else if (wifiSvc_ && wifiSvc_->isConnected) {
        netReady = wifiSvc_->isConnected(wifiSvc_->ctx);
    }
    if (!netReady) {
        (void)persistReceipt_(job.target, job.operationId, FirmwareUpdateReceiptState::Failed);
        setError_(job.target, "network not connected", job.operationId);
        return false;
    }

    char err[128] = {0};
    bool ok = false;
    switch (job.target) {
        case FirmwareUpdateTarget::Waveshare:
            ok = runWaveshareUpdate_(job, err, sizeof(err));
            break;
        case FirmwareUpdateTarget::Nextion:
            ok = runNextionUpdate_(job, err, sizeof(err));
            break;
        case FirmwareUpdateTarget::Spiffs:
            ok = runSpiffsUpdate_(job, err, sizeof(err));
            break;
        default:
            snprintf(err, sizeof(err), "unsupported target");
            ok = false;
            break;
    }

    if (!ok) {
        (void)persistReceipt_(job.target, job.operationId, FirmwareUpdateReceiptState::Failed);
        setError_(job.target, err[0] ? err : "update failed", job.operationId);
        LOGE("Update failed target=%s reason=%s", targetStr_(job.target), err[0] ? err : "unknown");
        return false;
    }

    LOGI("Update done target=%s", targetStr_(job.target));
    return true;
}

bool FirmwareUpdateModule::runManifestCheck_(const ManifestCheckJob& job,
                                             NextionArtifactSelection* nextionSelectionOut,
                                             size_t* payloadLenOut,
                                             char* errOut,
                                             size_t errOutLen)
{
    if (payloadLenOut) *payloadLenOut = 0U;
    if (nextionSelectionOut) *nextionSelectionOut = NextionArtifactSelection{};
    if (!manifestMetadata_ || !manifestPayload_) {
        writeSimpleError_(errOut, errOutLen, "manifest storage unavailable");
        return false;
    }

    if (!netAccessSvc_ && services_) {
        netAccessSvc_ = services_->get<NetworkAccessService>(ServiceId::NetworkAccess);
    }
    bool netReady = false;
    if (netAccessSvc_ && netAccessSvc_->isWebReachable) {
        netReady = netAccessSvc_->isWebReachable(netAccessSvc_->ctx);
    } else if (wifiSvc_ && wifiSvc_->isConnected) {
        netReady = wifiSvc_->isConnected(wifiSvc_->ctx);
    }
    if (!netReady) {
        writeSimpleError_(errOut, errOutLen, "network not connected");
        return false;
    }

    constexpr size_t kPayloadCapacity = Limits::FirmwareUpdate::Buffers::ManifestResponseJson;
    manifestPayload_[0] = '\0';

    HTTPClient http;
    configureDownloadHttp_(http);
    if (!http.begin(job.url)) {
        writeHttpBeginFailedError_("manifest", job.url, errOut, errOutLen);
        return false;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        writeHttpCodeFailedError_("manifest", job.url, http, code, errOut, errOutLen);
        http.end();
        return false;
    }

    const int announcedSize = http.getSize();
    if (announcedSize == 0) {
        writeSimpleError_(errOut, errOutLen, "manifest empty");
        http.end();
        return false;
    }
    if (announcedSize > 0 && (size_t)announcedSize > kPayloadCapacity) {
        writeSimpleError_(errOut, errOutLen, "manifest too large");
        http.end();
        return false;
    }

    BoundedBufferStream sink(manifestPayload_, kPayloadCapacity);
    const int written = http.writeToStream(&sink);
    const bool overflowed = sink.overflowed();
    const size_t payloadLen = sink.length();
    http.end();

    if (overflowed) {
        writeSimpleError_(errOut, errOutLen, "manifest too large");
        return false;
    }
    if (written < 0) {
        writeSimpleError_(errOut, errOutLen, "manifest read failed");
        return false;
    }
    if (payloadLen == 0U) {
        writeSimpleError_(errOut, errOutLen, "manifest empty");
        return false;
    }

    manifestPayload_[payloadLen] = '\0';
    size_t jsonCapacity = payloadLen + 1024U;
    if (jsonCapacity < Limits::FirmwareUpdate::Buffers::ManifestParseJson) {
        jsonCapacity = Limits::FirmwareUpdate::Buffers::ManifestParseJson;
    }
    // Manifest-check JSON may be larger than the paired-release manifest.
    // Keep its pool off the internal heap even when under the malloc threshold.
    SpiRamJsonDocument doc(jsonCapacity);
    if (doc.capacity() == 0U) {
        writeSimpleError_(errOut, errOutLen, "manifest json psram unavailable");
        return false;
    }
    LOGI("[OTA-PSRAM] stage=manifest_check_json pool_bytes=%lu internal=%lu largest=%lu psram=%lu",
         (unsigned long)doc.capacity(),
         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    const DeserializationError jsonErr =
        deserializeJson(doc, static_cast<const char*>(manifestPayload_), payloadLen);
    if (jsonErr || !doc.is<JsonObjectConst>()) {
        writeSimpleError_(errOut, errOutLen, "manifest invalid json");
        return false;
    }

    NextionArtifactSelection selection{};
    if (!hmiSvc_ && services_) {
        hmiSvc_ = services_->get<HmiService>(ServiceId::Hmi);
    }
    HmiDisplayIdentity identity{};
    const bool displayDetected =
        hmiSvc_ && hmiSvc_->getLocalDisplayIdentity &&
        hmiSvc_->getLocalDisplayIdentity(hmiSvc_->ctx, &identity);
    if (displayDetected) {
        snprintf(selection.displayModel, sizeof(selection.displayModel), "%s", identity.model);
        snprintf(selection.compatibility, sizeof(selection.compatibility), "%s", identity.compatibility);
    }

    const JsonVariantConst nextionValue = doc["artifacts"]["nextion"];
    if (!nextionValue.isNull()) {
        if (!nextionValue.is<JsonArrayConst>()) {
            writeSimpleError_(errOut, errOutLen, "manifest nextion artifacts must be an array");
            return false;
        }
        const JsonArrayConst artifacts = nextionValue.as<JsonArrayConst>();
        size_t artifactIndex = 0U;
        for (JsonObjectConst artifact : artifacts) {
            const char* path = artifact["path"].as<const char*>();
            const char* version = artifact["version"].as<const char*>();
            const char* compatibility = artifact["display_compatibility"].as<const char*>();
            const char* target = artifact["target"].as<const char*>();
            const char* kind = artifact["kind"].as<const char*>();
            const uint32_t size = artifact["size"] | 0U;

            char filenameCompatibility[HMI_DISPLAY_MODEL_TEXT_MAX]{};
            char filenameVersion[HMI_DISPLAY_VERSION_TEXT_MAX]{};
            if (!path || !version || !compatibility || size == 0U ||
                !target || strcmp(target, "nextion") != 0 ||
                !kind || strcmp(kind, "nextion-tft") != 0 ||
                !isSimpleArtifactFilename_(path) ||
                !parseNextionArtifactFilename(path,
                                              filenameCompatibility,
                                              sizeof(filenameCompatibility),
                                              filenameVersion,
                                              sizeof(filenameVersion)) ||
                strcmp(filenameCompatibility, compatibility) != 0 ||
                strcmp(filenameVersion, version) != 0) {
                writeSimpleError_(errOut, errOutLen, "manifest contains invalid nextion artifact");
                return false;
            }

            size_t previousIndex = 0U;
            for (JsonObjectConst previous : artifacts) {
                if (previousIndex++ >= artifactIndex) break;
                const char* previousCompatibility = previous["display_compatibility"] | "";
                const char* previousVersion = previous["version"] | "";
                if (strcmp(previousCompatibility, compatibility) == 0 &&
                    strcmp(previousVersion, version) == 0) {
                    writeSimpleError_(errOut, errOutLen, "manifest contains duplicate nextion artifact");
                    return false;
                }
            }
            ++artifactIndex;

            char artifactUrl[kUrlLen]{};
            if (!buildManifestSiblingUrl_(job.url, path, artifactUrl, sizeof(artifactUrl))) {
                writeSimpleError_(errOut, errOutLen, "nextion artifact url is invalid");
                return false;
            }

            NextionArtifactSelection artifactSelection{};
            artifactSelection.valid = true;
            snprintf(artifactSelection.compatibility,
                     sizeof(artifactSelection.compatibility),
                     "%s",
                     compatibility);
            snprintf(artifactSelection.path, sizeof(artifactSelection.path), "%s", path);
            snprintf(artifactSelection.version, sizeof(artifactSelection.version), "%s", version);
            snprintf(artifactSelection.url, sizeof(artifactSelection.url), "%s", artifactUrl);
            artifactSelection.size = size;
            // Optional for compatibility with older manifests. When supplied,
            // preverify the entire staged image before touching the display.
            const char* artifactShaText = artifact["sha256"] | "";
            if (artifactShaText[0] != '\0') {
                if (!parseSha256_(artifactShaText, artifactSelection.sha256)) {
                    writeSimpleError_(errOut, errOutLen, "invalid Nextion manifest SHA-256");
                    return false;
                }
                artifactSelection.hasSha256 = true;
            }
            if (!rememberNextionRecoveryArtifact_(artifactSelection, errOut, errOutLen)) return false;

            if (!displayDetected || strcmp(identity.compatibility, compatibility) != 0) continue;
            if (selection.valid && compareNextionVersions(version, selection.version) <= 0) continue;
            selection = artifactSelection;
            snprintf(selection.displayModel, sizeof(selection.displayModel), "%s", identity.model);
        }
    }

    if (nextionSelectionOut) *nextionSelectionOut = selection;

    if (payloadLenOut) *payloadLenOut = payloadLen;
    return true;
}

bool FirmwareUpdateModule::rememberNextionRecoveryArtifact_(
    const NextionArtifactSelection& artifact,
    char* errOut,
    size_t errOutLen)
{
    for (size_t i = 0U; i < nextionRecoveryArtifactCount_; ++i) {
        NextionArtifactSelection& remembered = manifestMetadata_->recoveryArtifacts[i];
        if (strcmp(remembered.compatibility, artifact.compatibility) != 0) continue;
        if (compareNextionVersions(artifact.version, remembered.version) > 0) {
            remembered = artifact;
        }
        return true;
    }

    if (nextionRecoveryArtifactCount_ >= kMaxNextionRecoveryArtifacts) {
        writeSimpleError_(errOut, errOutLen, "manifest contains too many nextion display families");
        return false;
    }
    manifestMetadata_->recoveryArtifacts[nextionRecoveryArtifactCount_++] = artifact;
    return true;
}

bool FirmwareUpdateModule::cmdStatus_(void* userCtx, const CommandRequest&, char* reply, size_t replyLen)
{
    FirmwareUpdateModule* self = static_cast<FirmwareUpdateModule*>(userCtx);
    if (!self) return false;
    if (!self->statusJson_(reply, replyLen)) {
        if (!writeErrorJson(reply, replyLen, ErrorCode::Failed, "fw.update.status")) {
            snprintf(reply, replyLen, "{\"ok\":false}");
        }
        return false;
    }
    return true;
}

bool FirmwareUpdateModule::cmdWaveshare_(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen)
{
    FirmwareUpdateModule* self = static_cast<FirmwareUpdateModule*>(userCtx);
    if (!self) return false;

    char url[kUrlLen] = {0};
    const char* explicitUrl = self->parseUrlArg_(req, url, sizeof(url)) ? url : nullptr;
    char err[120] = {0};
    uint32_t operationId = 0U;
    if (!self->startUpdate_(FirmwareUpdateTarget::Waveshare,
                            explicitUrl,
                            &operationId,
                            err,
                            sizeof(err))) {
        if (!writeErrorJson(reply, replyLen, ErrorCode::Failed, "fw.update.waveshare")) {
            snprintf(reply, replyLen, "{\"ok\":false}");
        }
        return false;
    }

    snprintf(reply,
             replyLen,
             "{\"ok\":true,\"queued\":true,\"target\":\"waveshare\",\"operation_id\":%lu}",
             (unsigned long)operationId);
    return true;
}

bool FirmwareUpdateModule::cmdNextion_(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen)
{
    FirmwareUpdateModule* self = static_cast<FirmwareUpdateModule*>(userCtx);
    if (!self) return false;

    char url[kUrlLen] = {0};
    const char* explicitUrl = self->parseUrlArg_(req, url, sizeof(url)) ? url : nullptr;
    char err[120] = {0};
    uint32_t operationId = 0U;
    if (!self->startUpdate_(FirmwareUpdateTarget::Nextion,
                            explicitUrl,
                            &operationId,
                            err,
                            sizeof(err))) {
        if (!writeErrorJson(reply, replyLen, ErrorCode::Failed, "fw.update.nextion")) {
            snprintf(reply, replyLen, "{\"ok\":false}");
        }
        return false;
    }

    snprintf(reply,
             replyLen,
             "{\"ok\":true,\"queued\":true,\"target\":\"nextion\",\"operation_id\":%lu}",
             (unsigned long)operationId);
    return true;
}

bool FirmwareUpdateModule::cmdNextionReboot_(void* userCtx, const CommandRequest&, char* reply, size_t replyLen)
{
    FirmwareUpdateModule* self = static_cast<FirmwareUpdateModule*>(userCtx);
    if (!self) return false;

    char err[120] = {0};
    if (!self->queueNextionReboot_(err, sizeof(err))) {
        sanitizeJsonString_(err);
        const int wrote = snprintf(reply,
                                   replyLen,
                                   "{\"ok\":false,\"err\":{\"code\":\"Failed\",\"where\":\"fw.nextion.reboot\",\"msg\":\"%s\"}}",
                                   err[0] ? err : "failed");
        return wrote > 0 && (size_t)wrote < replyLen;
    }

    snprintf(reply, replyLen, "{\"ok\":true,\"queued\":true,\"target\":\"nextion_reboot\"}");
    return true;
}

bool FirmwareUpdateModule::cmdSpiffs_(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen)
{
    FirmwareUpdateModule* self = static_cast<FirmwareUpdateModule*>(userCtx);
    if (!self) return false;

    char url[kUrlLen] = {0};
    const char* explicitUrl = self->parseUrlArg_(req, url, sizeof(url)) ? url : nullptr;
    char err[120] = {0};
    uint32_t operationId = 0U;
    if (!self->startUpdate_(FirmwareUpdateTarget::Spiffs,
                            explicitUrl,
                            &operationId,
                            err,
                            sizeof(err))) {
        if (!writeErrorJson(reply, replyLen, ErrorCode::Failed, "fw.update.spiffs")) {
            snprintf(reply, replyLen, "{\"ok\":false}");
        }
        return false;
    }

    snprintf(reply,
             replyLen,
             "{\"ok\":true,\"queued\":true,\"target\":\"spiffs\",\"operation_id\":%lu}",
             (unsigned long)operationId);
    return true;
}

void FirmwareUpdateModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    services_ = &services;
    cfgStore_ = &cfg;
    logHub_ = services.get<LogHubService>(ServiceId::LogHub);
    cmdSvc_ = services.get<CommandService>(ServiceId::Command);
    wifiSvc_ = services.get<WifiService>(ServiceId::Wifi);
    netAccessSvc_ = services.get<NetworkAccessService>(ServiceId::NetworkAccess);
    webInterfaceSvc_ = services.get<WebInterfaceService>(ServiceId::WebInterface);
    hmiSvc_ = services.get<HmiService>(ServiceId::Hmi);

    cfg.registerVar(updateHostVar_);
    cfg.registerVar(updatePathVar_);

    // Retained RTC breadcrumb: distinguishes intentional reboot from an
    // unexpected reset during the preceding OTA transfer.
    RebootDiagnostics::reportPreviousBoot();

    bootId_ = esp_random();
    if (bootId_ == 0U) bootId_ = 1U;
    loadLastFailure_();
    if (loadReceipt_()) {
        nextOperationId_ = lastReceipt_.operationId + 1U;
        if (nextOperationId_ == 0U) nextOperationId_ = 1U;
    } else {
        nextOperationId_ = esp_random();
        if (nextOperationId_ == 0U) nextOperationId_ = 1U;
    }

    void* metadataMemory = heap_caps_malloc(sizeof(ManifestMetadata), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (metadataMemory) {
        manifestMetadata_ = new (metadataMemory) ManifestMetadata{};
        LOGI("Manifest metadata ready bytes=%u memory=psram", (unsigned)sizeof(ManifestMetadata));
    } else {
        LOGE("Firmware updater unavailable: manifest metadata allocation failed bytes=%u",
             (unsigned)sizeof(ManifestMetadata));
    }

    constexpr size_t kManifestStorageBytes =
        Limits::FirmwareUpdate::Buffers::ManifestResponseJson + 1U;
    manifestPayload_ = static_cast<char*>(
        heap_caps_calloc(1, kManifestStorageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (!manifestPayload_) {
        LOGE("Manifest storage allocation failed bytes=%u", (unsigned)kManifestStorageBytes);
    } else {
        LOGI("Manifest storage ready bytes=%u%s",
             (unsigned)kManifestStorageBytes,
             " memory=psram"
        );
    }

    if (!services.add(ServiceId::FirmwareUpdate, &firmwareUpdateSvc_)) {
        LOGE("service registration failed: %s", toString(ServiceId::FirmwareUpdate));
    }

    if (cmdSvc_ && cmdSvc_->registerHandler) {
        cmdSvc_->registerHandler(cmdSvc_->ctx, "fw.update.status", &FirmwareUpdateModule::cmdStatus_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "fw.update.waveshare", &FirmwareUpdateModule::cmdWaveshare_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "fw.update.nextion", &FirmwareUpdateModule::cmdNextion_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "fw.nextion.reboot", &FirmwareUpdateModule::cmdNextionReboot_, this);
        cmdSvc_->registerHandler(cmdSvc_->ctx, "fw.update.spiffs", &FirmwareUpdateModule::cmdSpiffs_, this);
    }

    setStatus_(UpdateState::Idle, FirmwareUpdateTarget::Waveshare, 0, "idle");
    if (manifestMetadata_) LOGI("Firmware updater ready");
}

void FirmwareUpdateModule::loop()
{
    if (!manifestMetadata_) {
        delayNextCycle(pdMS_TO_TICKS(60));
        return;
    }

    static constexpr uint32_t kLocalReleaseIdleTimeoutMs = 120000U;
    bool localReleaseRebootDue = false;
    {
        // Upload callbacks run in the async TCP task. A non-blocking take keeps
        // the updater loop responsive while a flash chunk is being committed.
        SemaphoreGuard transactionGuard(localReleaseMutex_, 0U);
        if (transactionGuard.acquired()) {
            const uint32_t nowMs = millis();
            localReleaseRebootDue = localRelease_.rebootPending &&
                                    (int32_t)(nowMs - localRelease_.rebootAtMs) >= 0;
            if (localRelease_.active && !localRelease_.rebootPending &&
                localRelease_.lastActivityMs != 0U &&
                (uint32_t)(nowMs - localRelease_.lastActivityMs) > kLocalReleaseIdleTimeoutMs) {
                const uint32_t operationId = localRelease_.operationId;
                (void)persistReceipt_(FirmwareUpdateTarget::Waveshare,
                                      operationId,
                                      FirmwareUpdateReceiptState::Failed);
                resetLocalRelease_();
                setError_(FirmwareUpdateTarget::Waveshare,
                          "local release upload timed out",
                          operationId);
            }
        }
    }
    if (localReleaseRebootDue) {
        if (!prepareLocalReleaseReboot_()) return;
        LOGI("[UPGRADE] reboot");
        RebootDiagnostics::markPlanned(RebootDiagnostics::Origin::OtaRelease,
                                      localRelease_.operationId);
        ESP.restart();
    }

    UpdateJob job{};
    ManifestCheckJob manifestJob{};
    bool runNextionReboot = false;
    bool runManifestCheck = false;

    portENTER_CRITICAL(&lock_);
    if (busy_) {
        portEXIT_CRITICAL(&lock_);
        delayNextCycle(pdMS_TO_TICKS(60));
        return;
    }
    if (nextionRebootQueued_) {
        busy_ = true;
        nextionRebootQueued_ = false;
        runNextionReboot = true;
    } else if (queuedJob_.pending) {
        busy_ = true;
        job = queuedJob_;
        queuedJob_.pending = false;
    } else if (manifestCheckJob_.pending) {
        busy_ = true;
        manifestJob = manifestCheckJob_;
        manifestCheckJob_.pending = false;
        runManifestCheck = true;
        if (manifestMetadata_->check.requestId == manifestJob.requestId) {
            manifestMetadata_->check.state = FirmwareManifestCheckState::Downloading;
            manifestMetadata_->check.updatedAtMs = millis();
            snprintf(manifestMetadata_->check.message, sizeof(manifestMetadata_->check.message), "downloading");
        }
    } else {
        portEXIT_CRITICAL(&lock_);
        delayNextCycle(pdMS_TO_TICKS(60));
        return;
    }
    portEXIT_CRITICAL(&lock_);

    if (runNextionReboot) {
        char err[128] = {0};
        if (!runNextionReboot_(err, sizeof(err))) {
            LOGE("Nextion reboot failed reason=%s", err[0] ? err : "unknown");
        } else {
            LOGI("Nextion reboot done");
        }
    } else if (runManifestCheck) {
        size_t payloadLen = 0U;
        NextionArtifactSelection nextionSelection{};
        char err[128] = {0};
        const bool ok =
            runManifestCheck_(manifestJob, &nextionSelection, &payloadLen, err, sizeof(err));

        portENTER_CRITICAL(&lock_);
        if (manifestMetadata_->check.requestId == manifestJob.requestId) {
            manifestMetadata_->check.updatedAtMs = millis();
            if (ok) {
                manifestMetadata_->selection = nextionSelection;
                manifestMetadata_->check.state = FirmwareManifestCheckState::Ready;
                manifestMetadata_->check.payloadLen = payloadLen;
                manifestMetadata_->check.nextionDisplayDetected = nextionSelection.displayModel[0] != '\0';
                manifestMetadata_->check.nextionArtifactSelected = nextionSelection.valid;
                snprintf(manifestMetadata_->check.nextionDisplayModel,
                         sizeof(manifestMetadata_->check.nextionDisplayModel),
                         "%s",
                         nextionSelection.displayModel);
                snprintf(manifestMetadata_->check.nextionDisplayCompatibility,
                         sizeof(manifestMetadata_->check.nextionDisplayCompatibility),
                         "%s",
                         nextionSelection.compatibility);
                snprintf(manifestMetadata_->check.nextionArtifactPath,
                         sizeof(manifestMetadata_->check.nextionArtifactPath),
                         "%s",
                         nextionSelection.path);
                snprintf(manifestMetadata_->check.nextionArtifactVersion,
                         sizeof(manifestMetadata_->check.nextionArtifactVersion),
                         "%s",
                         nextionSelection.version);
                snprintf(manifestMetadata_->check.nextionArtifactUrl,
                         sizeof(manifestMetadata_->check.nextionArtifactUrl),
                         "%s",
                         nextionSelection.url);
                manifestMetadata_->check.nextionArtifactSize = nextionSelection.size;
                snprintf(manifestMetadata_->check.message, sizeof(manifestMetadata_->check.message), "ready");
            } else {
                manifestMetadata_->check.state = FirmwareManifestCheckState::Error;
                manifestMetadata_->check.payloadLen = 0U;
                snprintf(manifestMetadata_->check.message,
                         sizeof(manifestMetadata_->check.message),
                         "%s",
                         err[0] ? err : "manifest check failed");
            }
        }
        portEXIT_CRITICAL(&lock_);

        if (ok) {
            LOGI("Manifest check ready request=%lu bytes=%u",
                 (unsigned long)manifestJob.requestId,
                 (unsigned)payloadLen);
        } else {
            LOGE("Manifest check failed request=%lu reason=%s",
                 (unsigned long)manifestJob.requestId,
                 err[0] ? err : "unknown");
        }
    } else {
        runJob_(job);
    }

    portENTER_CRITICAL(&lock_);
    busy_ = false;
    activeTotalBytes_ = 0;
    activeSentBytes_ = 0;
    portEXIT_CRITICAL(&lock_);

    delayNextCycle(pdMS_TO_TICKS(20));
}

/**
 * @file FirmwareUpdateModule.cpp
 * @brief Firmware updater implementation.
 */

#include "FirmwareUpdateModule.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <FS.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ctype.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "App/BuildFlags.h"
#include "Board/BoardSpec.h"
#include "Core/BoundedBufferStream.h"
#include "Core/ErrorCodes.h"
#include "Core/FirmwareVersion.h"
#include "Core/NvsKeys.h"
#include "Core/SystemLimits.h"
#include "Modules/HMIModule/Drivers/NextionDisplayIdentity.h"

#include <ESPNexUpload.h>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::FirmwareUpdateModule)
#include "Core/ModuleLog.h"

namespace {

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

}  // namespace

FirmwareUpdateModule::FirmwareUpdateModule(const BoardSpec& board)
{
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
    if (!out || outLen == 0) return false;
    if (!msg) msg = "failed";
    const int n = snprintf(out, outLen, "%s", msg);
    return n > 0 && (size_t)n < outLen;
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
    portEXIT_CRITICAL(&lock_);

    const bool otaActive = state == UpdateState::Queued ||
                           state == UpdateState::Downloading ||
                           state == UpdateState::Flashing ||
                           state == UpdateState::Rebooting;
    setHmiOtaCondition_(otaActive);
}

void FirmwareUpdateModule::setError_(FirmwareUpdateTarget target, const char* msg, uint32_t operationId)
{
    setStatus_(UpdateState::Error, target, 0, msg ? msg : "failed", operationId);
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
    portENTER_CRITICAL(&lock_);
    snap = status_;
    busy = busy_;
    pending = queuedJob_.pending;
    hasLastReceipt = hasLastReceipt_;
    lastReceipt = lastReceipt_;
    portEXIT_CRITICAL(&lock_);

    sanitizeJsonString_(snap.msg);

    StaticJsonDocument<512> doc;
    doc["ok"] = true;
    doc["boot_id"] = bootId_;
    doc["operation_id"] = snap.operationId;
    doc["state"] = stateStr_(snap.state);
    doc["target"] = targetStr_(snap.target);
    doc["busy"] = busy;
    doc["pending"] = pending;
    doc["progress"] = snap.progress;
    doc["ts_ms"] = snap.updatedAtMs;
    doc["msg"] = snap.msg;
    if (hasLastReceipt) {
        JsonObject receipt = doc.createNestedObject("last_operation");
        receipt["operation_id"] = lastReceipt.operationId;
        receipt["target"] = targetStr_(lastReceipt.target);
        receipt["result"] = firmwareUpdateReceiptStateName(lastReceipt.state);
    } else {
        doc["last_operation"] = nullptr;
    }
    if (doc.overflowed()) return false;
    const size_t written = serializeJson(doc, out, outLen);
    return written > 0U && written < outLen;
}

bool FirmwareUpdateModule::isBusy_()
{
    bool busy = false;
    bool pending = false;
    bool nextionReboot = false;
    bool manifestCheckActive = false;
    bool updateStartPending = false;
    portENTER_CRITICAL(&lock_);
    busy = busy_;
    pending = queuedJob_.pending;
    nextionReboot = nextionRebootQueued_;
    manifestCheckActive = manifestCheckIsActive_(manifestCheck_.state);
    updateStartPending = updateStartPending_;
    portEXIT_CRITICAL(&lock_);
    return busy || pending || nextionReboot || manifestCheckActive || updateStartPending;
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

    if (!manifestPayload_) {
        writeSimpleError_(errOut, errOutLen, "manifest storage unavailable");
        return false;
    }

    portENTER_CRITICAL(&lock_);
    if (busy_ || queuedJob_.pending || nextionRebootQueued_ || updateStartPending_ ||
        manifestCheckIsActive_(manifestCheck_.state) || manifestCopyReaders_ > 0U) {
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

    manifestCheck_ = {};
    nextionSelection_ = {};
    manifestCheck_.requestId = nextManifestRequestId_;
    manifestCheck_.state = FirmwareManifestCheckState::Queued;
    manifestCheck_.updatedAtMs = millis();
    snprintf(manifestCheck_.manifestUrl, sizeof(manifestCheck_.manifestUrl), "%s", url);
    snprintf(manifestCheck_.message, sizeof(manifestCheck_.message), "queued");
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
    if (!out || requestId == 0U) return false;
    portENTER_CRITICAL(&lock_);
    if (manifestCheck_.requestId != requestId) {
        portEXIT_CRITICAL(&lock_);
        return false;
    }
    *out = manifestCheck_;
    portEXIT_CRITICAL(&lock_);
    return true;
}

bool FirmwareUpdateModule::copyManifestResult_(uint32_t requestId,
                                               char* out,
                                               size_t outLen,
                                               size_t* copiedLenOut)
{
    if (copiedLenOut) *copiedLenOut = 0U;
    if (!out || outLen == 0U || requestId == 0U || !manifestPayload_) return false;

    size_t payloadLen = 0U;
    portENTER_CRITICAL(&lock_);
    if (manifestCheck_.requestId != requestId ||
        manifestCheck_.state != FirmwareManifestCheckState::Ready ||
        manifestCheck_.payloadLen == 0U ||
        outLen <= manifestCheck_.payloadLen) {
        portEXIT_CRITICAL(&lock_);
        return false;
    }
    payloadLen = manifestCheck_.payloadLen;
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
    manifestCheckActive = manifestCheckIsActive_(manifestCheck_.state);
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

bool FirmwareUpdateModule::startUpdate_(FirmwareUpdateTarget target,
                                        const char* url,
                                        uint32_t* operationIdOut,
                                        char* errOut,
                                        size_t errOutLen)
{
    if (operationIdOut) *operationIdOut = 0U;
    UpdateJob job{};
    job.target = target;
    if (target == FirmwareUpdateTarget::Nextion) {
        NextionArtifactSelection selection{};
        portENTER_CRITICAL(&lock_);
        selection = nextionSelection_;
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
        if (!hmiSvc_ || !hmiSvc_->getLocalDisplayIdentity ||
            !hmiSvc_->getLocalDisplayIdentity(hmiSvc_->ctx, &identity)) {
            writeSimpleError_(errOut, errOutLen, "nextion display model not detected");
            return false;
        }
        if (!isNextionDisplayCompatible(identity, selection.compatibility)) {
            writeSimpleError_(errOut, errOutLen, "nextion display model changed since manifest check");
            return false;
        }

        snprintf(job.url, sizeof(job.url), "%s", selection.url);
        snprintf(job.nextionCompatibility,
                 sizeof(job.nextionCompatibility),
                 "%s",
                 selection.compatibility);
        job.expectedSize = selection.size;
    } else if (!resolveUrl_(target, url, job.url, sizeof(job.url), errOut, errOutLen)) {
        return false;
    }

    portENTER_CRITICAL(&lock_);
    if (busy_ || queuedJob_.pending || nextionRebootQueued_ || updateStartPending_ ||
        manifestCheckIsActive_(manifestCheck_.state) || manifestCopyReaders_ > 0U) {
        portEXIT_CRITICAL(&lock_);
        writeSimpleError_(errOut, errOutLen, "updater busy");
        return false;
    }
    updateStartPending_ = true;
    job.operationId = nextOperationId_++;
    if (nextOperationId_ == 0U) nextOperationId_ = 1U;
    portEXIT_CRITICAL(&lock_);

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
    if (nextionRebootPin_ < 0) {
        writeSimpleError_(errOut, errOutLen, "nextion reboot pin not configured");
        return false;
    }

    portENTER_CRITICAL(&lock_);
    if (busy_ || queuedJob_.pending || nextionRebootQueued_ || updateStartPending_ ||
        manifestCheckIsActive_(manifestCheck_.state) || manifestCopyReaders_ > 0U) {
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
    const char* url = job.url;
    setStatus_(UpdateState::Downloading,
               FirmwareUpdateTarget::Waveshare,
               0,
               "downloading",
               job.operationId);

    HTTPClient http;
    configureDownloadHttp_(http);
    if (!http.begin(url)) {
        writeHttpBeginFailedError_("fichier de mise a jour", url, errOut, errOutLen);
        return false;
    }

    const int code = http.GET();
    const int32_t contentLength = http.getSize();
    if (code != HTTP_CODE_OK) {
        writeHttpCodeFailedError_("fichier de mise a jour", url, http, code, errOut, errOutLen);
        http.end();
        return false;
    }

    setStatus_(UpdateState::Flashing,
               FirmwareUpdateTarget::Waveshare,
               0,
               "flashing",
               job.operationId);
    portENTER_CRITICAL(&lock_);
    activeTotalBytes_ = (contentLength > 0) ? (uint32_t)contentLength : 0U;
    activeSentBytes_ = 0;
    portEXIT_CRITICAL(&lock_);

    const esp_partition_t* runningPartition = esp_ota_get_running_partition();
    const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
    if (!updatePartition) {
        writeSimpleError_(errOut, errOutLen, "ota partition unavailable");
        http.end();
        return false;
    }
    if (runningPartition && updatePartition->address == runningPartition->address) {
        writeSimpleError_(errOut, errOutLen, "ota target equals running partition");
        http.end();
        return false;
    }
    if (contentLength > 0 && (size_t)contentLength > updatePartition->size) {
        writeSimpleError_(errOut, errOutLen, "ota image too large for partition");
        http.end();
        return false;
    }

    attachWebInterfaceSvcIfNeeded_();
    if (webInterfaceSvc_ && webInterfaceSvc_->setPaused) {
        webInterfaceSvc_->setPaused(webInterfaceSvc_->ctx, true);
    }

    char failMsg[128] = {0};
    const size_t beginSize = (contentLength > 0) ? (size_t)contentLength : (size_t)UPDATE_SIZE_UNKNOWN;
    if (!Update.begin(beginSize, U_FLASH)) {
        snprintf(failMsg, sizeof(failMsg), "ota begin failed (%u)", (unsigned)Update.getError());
    } else {
        auto* stream = http.getStreamPtr();
        int32_t remaining = contentLength;
        uint8_t buf[Limits::FirmwareUpdate::Http::StreamChunkBytes];
        uint32_t lastReadMs = millis();

        while (http.connected() && (contentLength <= 0 || remaining > 0)) {
            const size_t avail = stream ? stream->available() : 0;
            if (avail == 0U) {
                if (contentLength <= 0 && stream && !stream->connected()) {
                    break;
                }
                if ((millis() - lastReadMs) > Limits::FirmwareUpdate::Http::StreamReadTimeoutMs) {
                    snprintf(failMsg, sizeof(failMsg), "ota stream timeout");
                    break;
                }
                delay(1);
                continue;
            }

            const size_t toRead = (avail > sizeof(buf)) ? sizeof(buf) : avail;
            const int rd = stream->readBytes((char*)buf, toRead);
            if (rd <= 0) {
                delay(1);
                continue;
            }
            lastReadMs = millis();

            const size_t wr = Update.write(buf, (size_t)rd);
            if (wr != (size_t)rd) {
                snprintf(failMsg, sizeof(failMsg), "ota write failed (%u)", (unsigned)Update.getError());
                break;
            }

            onProgressChunk_((uint32_t)wr);

            if (contentLength > 0) {
                remaining -= rd;
                if (remaining <= 0) {
                    break;
                }
            }
        }

        if (failMsg[0] == '\0' && contentLength > 0 && remaining > 0) {
            snprintf(failMsg, sizeof(failMsg), "incomplete download");
        }
        if (failMsg[0] == '\0' && !Update.end()) {
            snprintf(failMsg, sizeof(failMsg), "ota end failed (%u)", (unsigned)Update.getError());
        }
        if (failMsg[0] == '\0' && !Update.isFinished()) {
            snprintf(failMsg, sizeof(failMsg), "ota not finished");
        }
    }

    if (webInterfaceSvc_ && webInterfaceSvc_->setPaused) {
        webInterfaceSvc_->setPaused(webInterfaceSvc_->ctx, false);
    }

    http.end();

    if (failMsg[0] != '\0') {
        writeSimpleError_(errOut, errOutLen, failMsg);
        return false;
    }

    if (!persistReceipt_(job.target,
                         job.operationId,
                         FirmwareUpdateReceiptState::RebootPending)) {
        writeSimpleError_(errOut, errOutLen, "failed to persist update completion");
        return false;
    }
    setStatus_(UpdateState::Rebooting,
               FirmwareUpdateTarget::Waveshare,
               100,
               "rebooting",
               job.operationId);
    delay(1800);
    ESP.restart();
    return true;
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
    // Compatibility is derived from the hardware model. The application
    // version is deliberately not required: a blank/unknown version must not
    // prevent recovery by uploading a compatible TFT.
    if (!hmiSvc_ || !hmiSvc_->getLocalDisplayIdentity ||
        !hmiSvc_->getLocalDisplayIdentity(hmiSvc_->ctx, &identity) ||
        !isNextionDisplayCompatible(identity, job.nextionCompatibility)) {
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
    const char* url = job.url;
    setStatus_(UpdateState::Downloading,
               FirmwareUpdateTarget::Spiffs,
               0,
               "downloading",
               job.operationId);

    HTTPClient http;
    configureDownloadHttp_(http);
    if (!http.begin(url)) {
        writeHttpBeginFailedError_("fichier de mise a jour", url, errOut, errOutLen);
        return false;
    }

    const int code = http.GET();
    const int32_t contentLength = http.getSize();
    if (code != HTTP_CODE_OK) {
        writeHttpCodeFailedError_("fichier de mise a jour", url, http, code, errOut, errOutLen);
        http.end();
        return false;
    }

    setStatus_(UpdateState::Flashing,
               FirmwareUpdateTarget::Spiffs,
               0,
               "flashing spiffs",
               job.operationId);
    portENTER_CRITICAL(&lock_);
    activeTotalBytes_ = (contentLength > 0) ? (uint32_t)contentLength : 0U;
    activeSentBytes_ = 0;
    portEXIT_CRITICAL(&lock_);

    attachWebInterfaceSvcIfNeeded_();
    if (webInterfaceSvc_ && webInterfaceSvc_->setPaused) {
        webInterfaceSvc_->setPaused(webInterfaceSvc_->ctx, true);
    }

    char failMsg[128] = {0};
    const size_t beginSize = (contentLength > 0) ? (size_t)contentLength : (size_t)UPDATE_SIZE_UNKNOWN;
    if (!Update.begin(beginSize, U_SPIFFS)) {
        snprintf(failMsg, sizeof(failMsg), "spiffs begin failed (%u)", (unsigned)Update.getError());
    }

    auto* stream = http.getStreamPtr();
    uint8_t buf[Limits::FirmwareUpdate::Http::StreamChunkBytes];
    int32_t remaining = contentLength;
    uint32_t lastReadMs = millis();
    if (failMsg[0] == '\0') {
        while (http.connected() && (contentLength <= 0 || remaining > 0)) {
            const size_t avail = stream ? stream->available() : 0;
            if (avail == 0U) {
                if (contentLength <= 0 && stream && !stream->connected()) {
                    break;
                }
                if ((millis() - lastReadMs) > Limits::FirmwareUpdate::Http::StreamReadTimeoutMs) {
                    snprintf(failMsg, sizeof(failMsg), "spiffs stream timeout");
                    break;
                }
                delay(1);
                continue;
            }

            const size_t toRead = (avail > sizeof(buf)) ? sizeof(buf) : avail;
            const int rd = stream->readBytes((char*)buf, toRead);
            if (rd <= 0) {
                delay(1);
                continue;
            }
            lastReadMs = millis();

            const size_t wr = Update.write(buf, (size_t)rd);
            if (wr != (size_t)rd) {
                snprintf(failMsg, sizeof(failMsg), "spiffs write failed (%u)", (unsigned)Update.getError());
                break;
            }

            onProgressChunk_((uint32_t)wr);

            if (contentLength > 0) {
                remaining -= rd;
                if (remaining <= 0) break;
            }
        }
    }
    http.end();

    if (failMsg[0] == '\0' && contentLength > 0 && remaining > 0) {
        snprintf(failMsg, sizeof(failMsg), "incomplete download");
    }
    if (failMsg[0] == '\0' && !Update.end()) {
        snprintf(failMsg, sizeof(failMsg), "spiffs end failed (%u)", (unsigned)Update.getError());
    }
    if (failMsg[0] == '\0' && !Update.isFinished()) {
        snprintf(failMsg, sizeof(failMsg), "spiffs not finished");
    }

    if (webInterfaceSvc_ && webInterfaceSvc_->setPaused) {
        webInterfaceSvc_->setPaused(webInterfaceSvc_->ctx, false);
    }

    if (failMsg[0] != '\0') {
        writeSimpleError_(errOut, errOutLen, failMsg);
        return false;
    }

    if (!persistReceipt_(job.target,
                         job.operationId,
                         FirmwareUpdateReceiptState::RebootPending)) {
        writeSimpleError_(errOut, errOutLen, "failed to persist update completion");
        return false;
    }
    setStatus_(UpdateState::Rebooting,
               FirmwareUpdateTarget::Spiffs,
               100,
               "rebooting",
               job.operationId);
    delay(1800);
    ESP.restart();
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
    if (!manifestPayload_) {
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
    DynamicJsonDocument doc(jsonCapacity);
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

            if (!displayDetected || strcmp(identity.compatibility, compatibility) != 0) continue;
            if (selection.valid && compareNextionVersions(version, selection.version) <= 0) continue;

            char artifactUrl[kUrlLen]{};
            if (!buildManifestSiblingUrl_(job.url, path, artifactUrl, sizeof(artifactUrl))) {
                writeSimpleError_(errOut, errOutLen, "nextion artifact url is invalid");
                return false;
            }
            selection.valid = true;
            snprintf(selection.path, sizeof(selection.path), "%s", path);
            snprintf(selection.version, sizeof(selection.version), "%s", version);
            snprintf(selection.url, sizeof(selection.url), "%s", artifactUrl);
            selection.size = size;
        }
    }

    if (nextionSelectionOut) *nextionSelectionOut = selection;

    if (payloadLenOut) *payloadLenOut = payloadLen;
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

    bootId_ = esp_random();
    if (bootId_ == 0U) bootId_ = 1U;
    if (loadReceipt_()) {
        nextOperationId_ = lastReceipt_.operationId + 1U;
        if (nextOperationId_ == 0U) nextOperationId_ = 1U;
    } else {
        nextOperationId_ = esp_random();
        if (nextOperationId_ == 0U) nextOperationId_ = 1U;
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
    LOGI("Firmware updater ready");
}

void FirmwareUpdateModule::loop()
{
    UpdateJob job{};
    ManifestCheckJob manifestJob{};
    bool runNextionReboot = false;
    bool runManifestCheck = false;

    portENTER_CRITICAL(&lock_);
    if (busy_) {
        portEXIT_CRITICAL(&lock_);
        vTaskDelay(pdMS_TO_TICKS(60));
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
        if (manifestCheck_.requestId == manifestJob.requestId) {
            manifestCheck_.state = FirmwareManifestCheckState::Downloading;
            manifestCheck_.updatedAtMs = millis();
            snprintf(manifestCheck_.message, sizeof(manifestCheck_.message), "downloading");
        }
    } else {
        portEXIT_CRITICAL(&lock_);
        vTaskDelay(pdMS_TO_TICKS(60));
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
        if (manifestCheck_.requestId == manifestJob.requestId) {
            manifestCheck_.updatedAtMs = millis();
            if (ok) {
                nextionSelection_ = nextionSelection;
                manifestCheck_.state = FirmwareManifestCheckState::Ready;
                manifestCheck_.payloadLen = payloadLen;
                manifestCheck_.nextionDisplayDetected = nextionSelection.displayModel[0] != '\0';
                manifestCheck_.nextionArtifactSelected = nextionSelection.valid;
                snprintf(manifestCheck_.nextionDisplayModel,
                         sizeof(manifestCheck_.nextionDisplayModel),
                         "%s",
                         nextionSelection.displayModel);
                snprintf(manifestCheck_.nextionDisplayCompatibility,
                         sizeof(manifestCheck_.nextionDisplayCompatibility),
                         "%s",
                         nextionSelection.compatibility);
                snprintf(manifestCheck_.nextionArtifactPath,
                         sizeof(manifestCheck_.nextionArtifactPath),
                         "%s",
                         nextionSelection.path);
                snprintf(manifestCheck_.nextionArtifactVersion,
                         sizeof(manifestCheck_.nextionArtifactVersion),
                         "%s",
                         nextionSelection.version);
                snprintf(manifestCheck_.nextionArtifactUrl,
                         sizeof(manifestCheck_.nextionArtifactUrl),
                         "%s",
                         nextionSelection.url);
                manifestCheck_.nextionArtifactSize = nextionSelection.size;
                snprintf(manifestCheck_.message, sizeof(manifestCheck_.message), "ready");
            } else {
                manifestCheck_.state = FirmwareManifestCheckState::Error;
                manifestCheck_.payloadLen = 0U;
                snprintf(manifestCheck_.message,
                         sizeof(manifestCheck_.message),
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

    vTaskDelay(pdMS_TO_TICKS(20));
}

#!/usr/bin/env node
/**
 * OTA/ZIP deep diagnostics regression contract.
 * Run: node scripts/tests/check_ota_deep_diagnostics.mjs
 * Verifies that instrumentation covers all critical transitions and remains
 * bounded. It is NOT a substitute for real ESP32-S3 OTA or a PlatformIO build.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const root = new URL("../../", import.meta.url);
const read = file => readFileSync(new URL(file, root), "utf8");
const fw = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const h = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.h");
const http = read("src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp");
const browser = read("data/webinterface/app.js");
let checks = 0;
const has = (s, phrase, why) => {
  ++checks;
  assert.ok(s.includes(phrase), why + ": " + phrase);
};
const ordered = (s, tokens, why) => {
  let previous = -1;
  for (const token of tokens) {
    const position = s.indexOf(token);
    assert.ok(position > previous, why + ": " + token);
    previous = position;
  }
  ++checks;
};
for (const tag of [
  "[OTA-ZIP-RPC] unready", "[OTA-ZIP-RPC] mutex_timeout",
  "[OTA-ZIP-RPC] gate_reject", "[OTA-ZIP-RPC] queue_timeout",
  "[OTA-ZIP-RPC] worker_stall", "[OTA-ZIP-RPC] reply",
  "[OTA-ZIP-WORK] start", "[OTA-ZIP-WORK] end",
  "[OTA-ZIP-WORK] completion",
  "[OTA-ZIP-PHASE] transaction_ready", "[OTA-ZIP-PHASE] image_prepare",
  "[OTA-ZIP-PHASE] image_ready", "[OTA-ZIP-PHASE] image_verify_begin",
  "[OTA-ZIP-PHASE] image_verify_ok",
  "[OTA-ZIP-PHASE] commit_begin", "[OTA-ZIP-PHASE] commit_ok",
  "[OTA-ZIP-CLEAN] abort_begin", "[OTA-ZIP-CLEAN] reset_begin",
  "[OTA-ZIP-CLEAN] reset_end", "[OTA-ZIP] abort cleanup",
  "[OTA-ZIP-FAIL]", "[OTA-ZIP-IDLE]", "[OTA-ADMIT] accepted",
  "[OTA-ADMIT] tgt="
]) has(fw, tag, "firmware diagnostic");
for (const tag of [
  "[OTA-ZIP-HTTP] manifest_accepted",
  "[OTA-ZIP-HTTP] manifest_rejected",
  "[OTA-ZIP-HTTP] image_open",
  "[OTA-ZIP-HTTP] image_begin_ok",
  "[OTA-ZIP-HTTP] image_progress",
  "[OTA-ZIP-HTTP] slow_callback",
  "[OTA-ZIP-HTTP] image_failed",
  "[OTA-ZIP-HTTP] image_verified",
  "[OTA-ZIP-HTTP] verification_failed",
  "[OTA-ZIP-HTTP] commit_ok",
  "[OTA-ZIP-HTTP] commit_failed",
  "[OTA-ZIP-HTTP] abort_ok",
  "[OTA-ZIP-HTTP] abort_failed",
  "[OTA-ZIP-HTTP] begin refused",
  "[OTA-ZIP-HTTP] chunk refused"
]) has(http, tag, "HTTP diagnostic");
for (const tag of [
  "[OTA-ZIP-BROWSER] image_start",
  "[OTA-ZIP-BROWSER] upload_progress",
  "[OTA-ZIP-BROWSER] image_end",
  "[OTA-ZIP-BROWSER] image_verified_response",
  "[OTA-ZIP-BROWSER] xhr_abort",
  "[OTA-ZIP-BROWSER] abort_response",
  "[OTA-ZIP-BROWSER] abort_network_failure",
  "[OTA-ZIP-BROWSER] manifest_accepted",
  "[OTA-ZIP-BROWSER] filesystem_verified",
  "[OTA-ZIP-BROWSER] firmware_verified",
  "[OTA-ZIP-BROWSER] commit_accepted"
]) has(browser, tag, "browser diagnostic");

has(h, "localReleaseLastIdleDiagMs_", "bounded idle watchdog");
has(fw, ">= 15000U", "watchdog every 15 seconds");
has(fw, ">= 10000U", "watchdog begins after 10s of no data");
has(fw, "roundTripMs >= 300U", "no log per normal RPC block");
has(fw, "workerMs >= 250U", "no log per normal worker chunk");
has(http, "writeMs >= 300U", "slow HTTP callback only");
has(http, "256U * 1024U", "HTTP 256 KiB milestones");
has(fw, "256U * 1024U", "Flash 256 KiB milestones");
has(browser, "nextCheckpoint = 25", "browser 25-percent milestones");
has(browser, "(Math.floor(percent / 25) + 1) * 25", "browser bounded milestones");
has(h, "localSlowRpcLastLogMs_", "RPC slow-chunk logging is rate-limited");
has(h, "localSlowWorkerLastLogMs_", "worker slow-chunk logging is rate-limited");
has(fw, "rpcNowMs - localSlowRpcLastLogMs_) >= 5000U", "RPC max one slow warning per 5s");
has(fw, "workerNowMs - localSlowWorkerLastLogMs_) >= 5000U", "worker max one slow warning per 5s");
has(http, "nowMs - context->lastSlowWarningMs) >= 5000U", "HTTP max one slow warning per 5s");
has(http, "++context->slowChunks", "all slow callbacks counted even when not logged");
has(fw, "xSemaphoreTake(localCallDone_, portMAX_DELAY)", "buffers held during long worker operation");
has(fw, "if (localCallPending_) {", "cancel only before worker starts");
has(fw, "localCallInFlight_ = false;", "RPC gate released");
has(fw, "localReleaseActive_ = false;", "local-release gate reset");
has(fw, "esp_ota_abort(localRelease_.otaHandle)", "flash handle reset on failure");
has(fw, "mbedtls_sha256_finish(&localRelease_.shaContext", "SHA validation intact");
has(fw, "esp_ota_set_boot_partition(target)", "A/B commit intact");
has(fw, "candidateFilesystemIsValid_(", "SPIFFS verification intact");
const rpc = fw.slice(fw.indexOf("bool FirmwareUpdateModule::dispatchLocalCall_("),
                     fw.indexOf("bool FirmwareUpdateModule::executeLocalCall_("));
ordered(rpc, ["portENTER_CRITICAL(&lock_);", "const uint32_t admissionMask",
              "portEXIT_CRITICAL(&lock_);", "if (otherJob) {"],
        "rejection gate snapshot captured under lock");
const loop = fw.slice(fw.indexOf("void FirmwareUpdateModule::loop()"));
ordered(loop, ["bool traceIdle = false;", "SemaphoreGuard transactionGuard",
               "if (traceIdle) {", 'LOGW("[OTA-ZIP-IDLE]'],
        "idle warning outside transaction guard");
const httpUpload = http.slice(http.indexOf("const auto registerLocalImageRoute"),
                              http.indexOf('registerLocalImageRoute("/api/upgrade/filesystem"'));
ordered(httpUpload, ["if (index == 0U)", "fwUpdateSvc_->beginLocalImage(",
                     "fwUpdateSvc_->writeLocalImage(", "if (context->ok) {",
                     'LOGI("[OTA-ZIP-HTTP] image_progress'],
        "HTTP callbacks log only accepted chunks");
const localZip = browser.slice(browser.indexOf("async function installLocalRelease("),
                               browser.indexOf("async function onWifiPageShown("));
ordered(localZip, ["uploadReleaseBlob('/api/upgrade/filesystem'",
                   "uploadReleaseBlob('/api/upgrade/firmware'",
                   "fetchOkJson('/api/upgrade/commit'"],
        "ZIP FS/firmware/commit sequence unchanged");
has(localZip, "await fetch('/api/upgrade/abort?transaction_id='",
    "abort still occurs after failed local ZIP");

// Model a 1572864 B SPIFFS and 2835456 B firmware transfer with 1436 B
// HTTP chunks; the count of diagnostic checkpoints must not follow chunk count.
function simulateMilestones(total, chunk = 1436) {
  let logs = 0;
  let chunks = 0;
  for (let offset = 0; offset < total; offset += chunk) {
    const received = Math.min(offset + chunk, total);
    ++chunks;
    if (offset === 0 || received === total ||
        Math.floor(offset / (256 * 1024)) !==
        Math.floor(received / (256 * 1024))) ++logs;
  }
  return { chunks, logs };
}
const spiffs = simulateMilestones(1572864);
const firmware = simulateMilestones(2835456);
assert.ok(spiffs.chunks > 1000 && firmware.chunks > 1900);
assert.ok(spiffs.logs <= 9 && firmware.logs <= 14);
assert.ok(spiffs.logs + firmware.logs < 25);
console.log("PASS: " + checks + " source contracts; " +
            (spiffs.chunks + firmware.chunks) + " simulated HTTP chunks, " +
            (spiffs.logs + firmware.logs) + " bounded progress markers");

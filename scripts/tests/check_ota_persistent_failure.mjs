#!/usr/bin/env node
/**
 * OTA last failure survives subsequent successes and is readable after boot.
 * Static source contract + deterministic behaviour examples; no hardware
 * flash or TCP simulation. Run with Node from any working directory.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const root = new URL("../../", import.meta.url);
const read = p => readFileSync(new URL(p, root), "utf8");
const h = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.h");
const cpp = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const policy = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateFailure.h");
const keys = read("include/Core/NvsKeys.h");
const modh = read("src/Modules/Network/ModbusTcpServerModule/ModbusTcpServerModule.h");
const modc = read("src/Modules/Network/ModbusTcpServerModule/ModbusTcpServerModule.cpp");
const log = read("src/Core/Services/ILogger.h");
const cap = Number(log.match(/constexpr int LOG_MSG_MAX = (\d+);/)?.[1]);
assert.equal(cap, 160, "Remeasure formatted logs after LogEntry changes");

assert.match(keys, /LastFailure\[\] = "fw_up_fail"/);
assert.match(policy, /static_assert\(sizeof\(Record\) <= 160U/);
assert.match(policy, /record\.checksum == checksum\(record\)/);
assert.match(policy, /record\.magic == kMagic/);
assert.match(policy, /record\.version == kVersion/);
assert.match(policy, /memchr\(record\.stage, 0, sizeof\(record\.stage\)\)/);
assert.match(policy, /memchr\(record\.reason, 0, sizeof\(record\.reason\)\)/);
assert.match(h, /OtaFailure::Record lastFailure_\{\}/);
assert.match(cpp, /void FirmwareUpdateModule::loadLastFailure_\(\)/);
assert.match(cpp, /void FirmwareUpdateModule::persistLastFailure_\(/);
assert.match(cpp, /cfgStore_->readRuntimeBlob\(NvsKeys::FirmwareUpdate::LastFailure/);
assert.match(cpp, /cfgStore_->writeRuntimeBlob\(\s*NvsKeys::FirmwareUpdate::LastFailure/);
assert.match(cpp, /loadLastFailure_\(\);\s*if \(loadReceipt_\(\)\)/);
assert.match(cpp, /\[OTA-LASTFAIL\] retained=1/);
assert.match(cpp, /\[OTA-LASTFAIL\] persisted=%u/);
assert.match(cpp, /\[OTA-LASTFAIL\] reason=%.95s/);
assert.match(cpp, /lastFailure = lastFailure_;/);
assert.match(cpp, /createNestedObject\("last_failure"\)/);
assert.match(cpp, /if \(measureJson\(doc\) >= outLen\) doc\.remove\("last_failure"\)/);
const errorSetter = cpp.split("void FirmwareUpdateModule::setError_(")[1]
  ?.split("void FirmwareUpdateModule::loadLastFailure_(")[0];
assert.ok(errorSetter);
assert.ok(errorSetter.indexOf("status_.otaStage") <
          errorSetter.indexOf("setStatus_(UpdateState::Error"),
          "Capture the phase before error status clears it");
assert.ok(errorSetter.includes("persistLastFailure_("));
const failureBlock = cpp.split('    char failedStage[sizeof(status_.otaStage)]{};')[1]
  ?.split('    remoteReleaseWorker_ = nullptr;')[0];
assert.ok(failureBlock);
const phaseCapture = failureBlock.indexOf("snprintf(failedStage,");
const abortCall = failureBlock.indexOf("(void)abortLocalRelease_(");
assert.ok(phaseCapture >= 0 && abortCall >= 0, "Phase capture and abort call must exist");
assert.ok(phaseCapture < abortCall,
          "Store failed phase before abort sets state=idle");
assert.ok(failureBlock.lastIndexOf("persistLastFailure_(") >
          abortCall,
          "Persistent diagnostic must run only after abort");
const persist = cpp.split("void FirmwareUpdateModule::persistLastFailure_(")[1]
  ?.split("bool FirmwareUpdateModule::loadReceipt_(")[0];
assert.ok(persist);
assert.match(persist, /hasLastFailure_ && lastFailure_\.operationId == operationId/);
assert.match(persist, /writeRuntimeBlob/);
const successPath = cpp.split("bool FirmwareUpdateModule::runJob_(")[1]
  ?.split("bool FirmwareUpdateModule::runManifestCheck_(")[0];
assert.ok(successPath);
assert.doesNotMatch(successPath, /eraseKey\(NvsKeys::FirmwareUpdate::LastFailure/);
assert.doesNotMatch(successPath, /lastFailure_ = \{\}/);
assert.match(cpp, /flashError = esp_ota_begin\(localRelease_\.writePartition, totalSize, &localRelease_\.otaHandle\);/);
assert.match(cpp, /stage=preverify_end sha256=%s ms=%lu flash_touched=0/);
assert.match(cpp, /RebootDiagnostics::markPlanned\(RebootDiagnostics::Origin::OtaRelease/);

assert.match(modh, /\+\+otaSuspensions_/);
assert.match(modh, /\+\+otaResumptions_/);
assert.match(modh, /uint32_t otaSuspensions_ = 0U/);
assert.match(modh, /uint32_t otaResumptions_ = 0U/);
const pause = modh.indexOf("++otaSuspensions_");
assert.ok(pause > modh.indexOf("server_.stop();"), "Only count actual stop");
const resumed = modh.indexOf("++otaResumptions_");
assert.ok(resumed > modh.indexOf("server_.begin("), "Only count after a successful start");
assert.match(modc, /\[MODBUS-OTA\] paused=%lu resumed=%lu pending=%u busy=%u listen=%u/);
assert.doesNotMatch(modc, /reconnect_attempts=%/, "Closed TCP listener cannot see SYN retries");
// No per-frame metrics; the current call happens at the one-minute baseline
// and five-second OTA snapshot, never in the Modbus request handler.
assert.match(modh, /logWebSnapshot_\(now, stationConnected, true\)/);
assert.match(modh, /logWebSnapshot_\(now, stationConnected, false\)/);
assert.match(modh, /uint32_t\(now - lastOtaTraceMs_\) >= 5000U/);
assert.match(modh, /uint32_t\(now - lastWebDiagnosticsMs_\) >= 60000U/);

const formats = [
  "[OTA-LASTFAIL] retained=1 op=%lu code=%u target=%u stage=%.27s",
  "[OTA-LASTFAIL] persisted=%u op=%lu code=%u target=%u stage=%.27s",
  "[OTA-LASTFAIL] reason=%.95s",
  "[MODBUS-OTA] paused=%lu resumed=%lu pending=%u busy=%u listen=%u",
];
for (const format of formats) {
  assert.ok(cpp.includes(format) || modc.includes(format),
    "Missing " + format);
  const length = format.replace(/%lu/g, "4294967295")
    .replace(/%u/g, "65535")
    .replace(/%\.27s/g, "x".repeat(27))
    .replace(/%\.95s/g, "y".repeat(95)).length;
  assert.ok(length < cap, "Log would be truncated: " + format);
}
// Recording error for op1 must survive success of op2; only a new failed
// operation supersedes it. The firmware records by operation ID, not RTC.
function simulateSequence() {
  let failure = null;
  const fail = (operationId, stage) => {
    if (failure?.operationId !== operationId) failure = {operationId, stage};
  };
  fail(11, "firmware_flash");
  fail(11, "idle"); // duplicate error from runJob_, ignored
  assert.deepEqual(failure, {operationId:11, stage:"firmware_flash"});
  const success = () => {}; // no clearing in success path
  success();
  assert.deepEqual(failure, {operationId:11, stage:"firmware_flash"});
  fail(13, "spiffs_verify");
  assert.deepEqual(failure, {operationId:13, stage:"spiffs_verify"});
}
simulateSequence();
console.log("PASS: persistent OTA last failure, stage retained after abort, Modbus pause/reopen counters");

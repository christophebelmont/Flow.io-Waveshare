#!/usr/bin/env node
/**
 * ZIP release A/B integration contract (source-level).
 *
 * Run: node scripts/tests/check_zip_ab_release.mjs
 * Checks browser ZIP upload -> HTTP routes -> transaction engine ->
 * inactive partitions -> verified commit -> reboot and post-boot confirmation.
 * Does not replace a real ZIP upload on ESP32-S3 hardware.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const root = new URL("../../", import.meta.url);
const read = name => readFileSync(new URL(name, root), "utf8");
const js = read("data/webinterface/app.js");
const server = read("src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp");
const updater = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const release = read("src/Core/ReleaseStorage.cpp");
const bootstrap = read("src/App/Bootstrap.cpp");
const exporter = read("scripts/export_binaries.py");
const partitions = read("partition_waveshare_ota_16mb.csv");
const spiffsBuild = read("scripts/prepare_spiffs_data.py");

function has(text, part, label) { assert.ok(text.includes(part), label); }
function ordered(text, parts, label) {
  let prev = -1;
  for (const part of parts) {
    const index = text.indexOf(part);
    assert.ok(index > prev, label + " (" + part + ")");
    prev = index;
  }
}

const zipStart = js.indexOf("async function installLocalRelease(file)");
const zipEnd = js.indexOf("async function onWifiPageShown()", zipStart);
assert.ok(zipStart > 0 && zipEnd > zipStart);
const zip = js.slice(zipStart, zipEnd);

has(js, "['manifest.json', 'firmware.bin', 'spiffs.bin']", "ZIP three-entry contract");
ordered(zip, [
  "fetchOkJson('/api/upgrade/begin'",
  "uploadReleaseBlob('/api/upgrade/filesystem'",
  "uploadReleaseBlob('/api/upgrade/firmware'",
  "fetchOkJson('/api/upgrade/commit'"
], "ZIP upload/commit order");
has(zip, "'/api/upgrade/abort?transaction_id='", "abort on failure");
has(server, '"/api/upgrade/begin"', "begin endpoint");
has(server, "fwUpdateSvc_->beginLocalRelease(", "begin service binding");
has(server, 'registerLocalImageRoute("/api/upgrade/filesystem", FirmwareUpdateTarget::Spiffs)', "filesystem route");
has(server, 'registerLocalImageRoute("/api/upgrade/firmware", FirmwareUpdateTarget::Waveshare)', "firmware route");
has(server, "fwUpdateSvc_->writeLocalImage(", "stream write service binding");
has(server, "fwUpdateSvc_->endLocalImage(", "image SHA validation binding");
has(server, '"/api/upgrade/image/status"', "asynchronous Flash progress route");
has(server, 'request->send(202, "application/json"', "upload callback must not wait for Flash");
has(zip, "await waitForZipImageFlash(transactionId, 'spiffs');", "wait for SPIFFS Flash before firmware");
has(zip, "await waitForZipImageFlash(transactionId, 'firmware');", "wait for firmware Flash before commit");
has(server, "fwUpdateSvc_->commitLocalRelease(", "commit binding");

has(release, "gRunningSlot = slotFromPartition(running);", "running slot from running app");
has(release, "return gRunningSlot == ReleaseSlot::A ? ReleaseSlot::B : ReleaseSlot::A;", "inactive slot toggles");
has(release, "SPIFFS.begin(false, \"/spiffs\", 10, filesystemLabel(gRunningSlot))", "boot mounts matching filesystem");
has(updater, "candidate.targetSlot = ReleaseStorage::inactiveSlot();", "write target inactive");
has(updater, "ESP_PARTITION_SUBTYPE_APP_OTA_0", "OTA app0 subtype");
has(updater, "ESP_PARTITION_SUBTYPE_APP_OTA_1", "OTA app1 subtype");
has(updater, 'failLocalRelease_("release target overlaps active partition")', "never overwrite active slot");
has(updater, "esp_partition_write(localRelease_.writePartition, offset, data, len)", "SPIFFS write");
has(updater, "esp_ota_write(localRelease_.otaHandle, data, len)", "firmware write");
has(updater, 'failLocalRelease_("release image sha256 mismatch")', "both image SHA256 checks");
has(updater, "candidateFilesystemIsValid_(", "candidate SPIFFS validation");
has(updater, "localRelease_.stage != LocalReleaseStage::FirmwareVerified", "require both images verified");
has(updater, "esp_ota_set_boot_partition(target)", "activate new boot partition");
has(updater, "OTA_WITH_SEQUENTIAL_WRITES", "ZIP defers firmware erase until write");
has(updater, "esp_ota_begin(localRelease_.writePartition,\n                            static_cast<size_t>(OTA_WITH_SEQUENTIAL_WRITES)", "local ZIP uses incremental erase in background");
has(updater, "[OTA-ZIP-PSRAM] flash_readback_sha256", "post-flash SHA-256 integrity check");
const beginImageStart = updater.indexOf("bool FirmwareUpdateModule::beginLocalImage_(");
const beginImageEnd = updater.indexOf("bool FirmwareUpdateModule::writeLocalImage_(", beginImageStart);
const beginImageCode = updater.slice(beginImageStart, beginImageEnd);
has(beginImageCode, "const bool remotePaired = remoteReleaseWorker_ != nullptr", "remote paired OTA stays distinct");
has(beginImageCode, "flashError = esp_ota_begin(localRelease_.writePartition,", "SDK OTA initialization retained");
has(beginImageCode, "totalSize, &localRelease_.otaHandle)", "remote OTA retains original upfront erase");
has(beginImageCode, "OtaPsramPolicy::canStageImage(totalSize, freeBytes, largestBlock)", "ZIP PSRAM 2MiB safety reserve");
assert.ok(!beginImageCode.includes("esp_ota_begin(localRelease_.writePartition, totalSize, &localRelease_.otaHandle)"),
          "local ZIP no longer triggers a full synchronous erase");

has(updater, "localRelease_.rebootPending = true;", "schedule single reboot");
has(release, "esp_ota_mark_app_invalid_rollback_and_reboot()", "post-boot rollback");
has(bootstrap, "ReleaseStorage::confirmPendingApplication()", "confirm only after module startup");

const endStart = updater.indexOf("bool FirmwareUpdateModule::endLocalImage_(");
const commitStart = updater.indexOf("bool FirmwareUpdateModule::commitLocalRelease_(", endStart);
assert.ok(endStart > 0 && commitStart > endStart);
const endImage = updater.slice(endStart, commitStart);
assert.doesNotMatch(endImage, /setStatus_\(UpdateState::Done/, "individual ZIP image must not claim release is done");
assert.match(endImage, /setStatus_\(UpdateState::Flashing,\s*FirmwareUpdateTarget::Waveshare/,
  "ZIP upload remains flashing until final paired commit");
has(exporter, 'zipfile.ZIP_STORED', "ZIP entries uncompressed");
has(exporter, '"sha256": _sha256(firmware)', "firmware digest");
has(exporter, '"sha256": _sha256(filesystem)', "SPIFFS digest");
has(exporter, "filesystem.stat().st_size != expected_filesystem_size", "SPIFFS partition size");
has(spiffsBuild, 'staging_dir / "ota-paired-v1.marker"', "paired release marker");
for (const name of ["app0", "app1", "spiffs0", "spiffs1"]) {
  assert.ok(partitions.split("\n").some(line => line.startsWith(name + ",")), "partition " + name);
}

// Local ZIP browser callbacks run in AsyncTCP, but every Flash/FS operation
// must run on the existing fwupdate task with a larger internal stack.
const header = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.h");
const localCalls = ["beginLocalRelease_", "beginLocalImage_", "writeLocalImage_",
                    "endLocalImage_", "commitLocalRelease_", "abortLocalRelease_"];
for (const name of localCalls) {
    const start = updater.indexOf("bool FirmwareUpdateModule::" + name + "(");
    assert.ok(start > 0, "missing " + name);
    const opening = updater.indexOf("{", start);
    const beginning = updater.slice(opening, opening + 820);
    has(beginning, "xTaskGetCurrentTaskHandle() != localCallOwner_", name + " routes other tasks");
    has(beginning, "dispatchLocalCall_(", name + " dispatch");
    assert.ok(beginning.indexOf("dispatchLocalCall_(") <
              beginning.indexOf("SemaphoreGuard transactionGuard(localReleaseMutex_)"),
              name + " must dispatch before the flash transaction lock");
}
has(header, "LocalCallKind", "worker API");
has(header, "localCallPending_", "one-at-a-time local RPC");
has(header, "localCallInFlight_", "remote admission gate");
has(updater, "xSemaphoreCreateBinaryStatic(&localCallDoneStorage_)", "static completion semaphore");
has(updater, "handleQueuedLocalCall_()", "firmware worker processing");
has(updater, "xSemaphoreGive(localCallDone_)", "worker completion");
has(updater, "const bool foreignLocalRpc = localCallInFlight_", "pending ZIP excludes other update requests");
has(updater, "if (localCallPending_) {", "timeout cancellation before worker takes request");
has(updater, "xSemaphoreTake(localCallDone_, portMAX_DELAY)", "never free request buffer while worker uses it");
const loopIndex = updater.indexOf("void FirmwareUpdateModule::loop()");
const loopHead = updater.slice(loopIndex, loopIndex + 1200);
has(loopHead, "handleQueuedLocalCall_()", "fwupdate task handles local ZIP");
assert.ok(loopHead.indexOf("handleQueuedLocalCall_()") <
          loopHead.indexOf("if (!manifestMetadata_)"), "worker callbacks before updater idle returns");

// Post-abort observability: a failed ZIP must be distinguishable from an OTA
// admission rejection and from a TCP transport error after cleanup.
const jsPos = js.indexOf("function uploadReleaseBlob(");
const jsEnd = js.indexOf("async function installLocalRelease(", jsPos);
const uploadClient = js.slice(jsPos, jsEnd);
const serverZipPos = server.indexOf("const auto registerLocalImageRoute");
const serverZip = server.slice(serverZipPos,
  server.indexOf('registerLocalImageRoute("/api/upgrade/filesystem"', serverZipPos));
has(updater, "[OTA-ADMIT] tgt=%u", "new remote OTA admission diagnostics");
has(updater, "[OTA-ZIP] abort cleanup op=", "local abort cleanup diagnostics");
has(updater, "[OTA-ZIP] progress target=", "bounded image upload progress");
has(serverZip, "[OTA-ZIP-HTTP] image_failed", "HTTP response failure");
has(serverZip, "[OTA-ZIP-HTTP] begin refused", "image begin failure");
has(serverZip, "[OTA-ZIP-HTTP] chunk refused", "image chunk error");
has(serverZip, "[OTA-ZIP-HTTP] verification_failed", "image verification error");
has(uploadClient, "[OTA-ZIP-HTTP] refus du serveur", "browser HTTP error details");
has(uploadClient, "request.onabort", "browser abort marker");
const startUpdatePos = updater.indexOf("bool FirmwareUpdateModule::startUpdate_(");
const startUpdateEnd = updater.indexOf("bool FirmwareUpdateModule::", startUpdatePos + 30);
const startUpdate = updater.slice(startUpdatePos, startUpdateEnd);
const snapshot = startUpdate.indexOf("const unsigned state = (unsigned)status_.state;");
const unlock = startUpdate.indexOf("portEXIT_CRITICAL(&lock_);", snapshot);
const warn = startUpdate.indexOf("LOGW(\"[OTA-ADMIT]", unlock);
assert.ok(snapshot >= 0 && unlock > snapshot && warn > unlock,
          "log after releasing OTA admission spinlock");

console.log("PASS: ZIP upload uses inactive paired app/SPIFFS A/B slots; no premature Done before commit");

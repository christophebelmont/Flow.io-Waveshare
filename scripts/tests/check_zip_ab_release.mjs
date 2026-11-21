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
console.log("PASS: ZIP upload uses inactive paired app/SPIFFS A/B slots; no premature Done before commit");

#!/usr/bin/env node
/**
 * ZIP PSRAM preverify + nonblocking Flash regression.
 *
 * Run: node scripts/tests/check_zip_psram_preverify_async.mjs
 * Source-level contracts + fault-injected state simulations.
 * A full PlatformIO build and a physical ESP32-S3 upload are still required.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const base = new URL("../../", import.meta.url);
const read = p => readFileSync(new URL(p, base), "utf8");
const fw = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const head = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.h");
const svc = read("src/Core/Services/IFirmwareUpdate.h");
const http = read("src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp");
const browser = read("data/webinterface/app.js");
const policy = read("src/Core/OtaPsramPolicy.h");
let checks = 0;
const has = (source, token, message) => {
  ++checks;
  assert.ok(source.includes(token), message + ": " + token);
};
const before = (source, first, second, message) => {
  ++checks;
  const a = source.indexOf(first), b = source.indexOf(second);
  assert.ok(a >= 0 && b > a, message + " (" + first + " before " + second + ")");
};
const part = (start, end) => {
  const a = fw.indexOf(start), b = fw.indexOf(end, a + start.length);
  assert.ok(a >= 0 && b > a, "code section " + start);
  return fw.slice(a, b);
};

const begin = part("bool FirmwareUpdateModule::beginLocalImage_(",
                   "bool FirmwareUpdateModule::writeLocalImage_(");
const receive = part("bool FirmwareUpdateModule::writeLocalImage_(",
                     "bool FirmwareUpdateModule::endLocalImage_(");
const finish = part("bool FirmwareUpdateModule::endLocalImage_(",
                    "bool FirmwareUpdateModule::commitLocalRelease_(");
const flash = part("bool FirmwareUpdateModule::flashStagedLocalImage_(",
                   "void FirmwareUpdateModule::resetLocalRelease_()");
const status = part("bool FirmwareUpdateModule::localImageProgressSnapshot_(",
                    "void FirmwareUpdateModule::setLocalImageProgress_(");
const loop = fw.slice(fw.indexOf("void FirmwareUpdateModule::loop()"));
const zip = browser.slice(browser.indexOf("async function installLocalRelease("),
                          browser.indexOf("async function onWifiPageShown("));
for (const token of [
  "OtaPsramPolicy::canStageImage(totalSize, freeBytes, largestBlock)",
  "heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)",
  "heap_caps_malloc(totalSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)",
  'failLocalRelease_("ZIP PSRAM insufficient (2 MiB reserve)")',
  "localRelease_.stagedBytes = totalSize"
]) has(begin, token, "allocate single ZIP image with reserve");
for (const token of [
  "memcpy(localRelease_.stagedImage + offset, data, len)",
  "mbedtls_sha256_update(&localRelease_.shaContext, data, len)",
  "len > localRelease_.stagedBytes - offset",
  "offset != localRelease_.received"
]) has(receive, token, "receive into PSRAM with bounds checks");
has(receive, "if (localRelease_.stagedImage)", "ZIP does not write incoming chunks to Flash");
before(finish, "sha256Matches_(digest, image.sha256)",
               "FirmwareLocalImagePhase::Queued", "SHA verification before queue");
has(finish, "flash_touched=0", "no preverify Flash changes");
assert.ok(!finish.slice(0, finish.indexOf("FirmwareLocalImagePhase::Queued")).includes("esp_partition_erase_range"),
          "no Flash erase during PSRAM preverify");
has(flash, "esp_partition_erase_range(localRelease_.writePartition", "inactive SPIFFS erase");
has(flash, "OTA_WITH_SEQUENTIAL_WRITES", "incremental firmware erase");
has(flash, "memcpy(chunk, localRelease_.stagedImage + written, amount)", "internal Flash buffer");
has(flash, "esp_partition_write(localRelease_.writePartition, written, chunk, amount)", "SPIFFS Flash");
has(flash, "esp_ota_write(localRelease_.otaHandle, chunk, amount)", "firmware Flash");
has(flash, "esp_ota_end(localRelease_.otaHandle)", "firmware validation");
has(flash, "esp_partition_read(localRelease_.writePartition", "read-back Flash");
has(flash, "sha256Matches_(digest, expected.sha256)", "post-write SHA validation");
before(flash, "flash_readback_sha256", "FirmwareLocalImagePhase::Verified",
       "only publish success after readback");
has(flash, "heap_caps_free(localRelease_.stagedImage)", "release PSRAM on success");
has(fw, "if (localRelease_.stagedImage) {\n        heap_caps_free(localRelease_.stagedImage);",
    "release PSRAM on abort or failure");
has(head, "localFlashStarted_", "prevent duplicate queued Flash");
has(status, "!localFlashQueued_ && !localFlashStarted_", "poll handshake only once");
has(status, "localFlashStartAfterMs_ = millis() + 250U", "give HTTP GET a grace period");
has(loop, "localFlashStarted_ = true", "transition to running before unlocking");
has(loop, "flashStagedLocalImage_()", "background worker processes staged image");
has(http, 'request->send(202, "application/json", "{\\\"ok\\\":true,\\\"staged\\\":true}")',
    "POST returns 202 on preverify");
has(http, '"/api/upgrade/image/status"', "dedicated nonblocking snapshot API");
has(svc, "FirmwareLocalImageProgress", "service snapshot type");
has(head, "bind<&FirmwareUpdateModule::localImageProgressSnapshot_>", "service snapshot binding");
has(zip, "await waitForZipImageFlash(transactionId, 'spiffs')", "wait for SPIFFS after staging");
has(zip, "await waitForZipImageFlash(transactionId, 'firmware')", "wait for firmware after staging");
before(zip, "uploadReleaseBlob('/api/upgrade/filesystem'", "await waitForZipImageFlash(transactionId, 'spiffs')",
       "stage SPIFFS first");
before(zip, "await waitForZipImageFlash(transactionId, 'spiffs')", "uploadReleaseBlob('/api/upgrade/firmware'",
       "do not start firmware early");
before(zip, "await waitForZipImageFlash(transactionId, 'firmware')", "fetchOkJson('/api/upgrade/commit'",
       "commit only after firmware Flash success");
has(policy, "constexpr size_t kReserveBytes = 2U * 1024U * 1024U", "2 MiB safety reserve");
has(fw, "esp_ota_set_boot_partition(target)", "A/B activation unchanged");
has(fw, "candidate.targetSlot = ReleaseStorage::inactiveSlot()", "slot setup");

const MiB = 1024 * 1024;
class SimulatedZip {
  constructor(free = 5_672_000, largest = 5_610_000) {
    this.free = free; this.largest = largest; this.reserve = 2 * MiB;
    this.stage = "prepared"; this.flashTouched = false;
    this.queued = false; this.started = false; this.psram = 0;
  }
  begin(target, size) {
    if (this.stage !== "prepared" && this.stage !== "spiffs-verified") return false;
    if (size === 0 || this.free <= this.reserve ||
        size > this.free - this.reserve || size > this.largest) {
      this.stage = "error"; return false;
    }
    this.target = target; this.size = size; this.received = 0;
    this.free -= size; this.psram = size; this.stage = "receiving";
    this.queued = false; this.started = false; return true;
  }
  chunk(n, offset) {
    if (this.stage !== "receiving" || offset !== this.received ||
        n <= 0 || n > this.size - this.received) {
      this.stage = "error"; return false;
    }
    this.received += n; return true;
  }
  preverify(sha) {
    if (this.stage !== "receiving" || this.received !== this.size || !sha) {
      this.stage = "error"; return false;
    }
    this.stage = "queued"; return true;
  }
  ack() {
    if (this.stage === "queued" && !this.queued && !this.started) {
      this.queued = true; return true;
    }
    return false;
  }
  flashResult(ok, flashHashOk) {
    if (!this.queued || this.stage !== "queued") return false;
    this.queued = false; this.started = true; this.stage = "flashing";
    this.flashTouched = true;
    if (!ok || !flashHashOk) { this.stage = "error"; return false; }
    this.psram = 0; this.free += this.size;
    this.stage = this.target === "spiffs" ? "spiffs-verified" : "firmware-verified";
    return true;
  }
  commit() {
    if (this.stage !== "firmware-verified") return false;
    this.stage = "reboot"; return true;
  }
  abort() { this.free += this.psram; this.psram = 0; this.queued = false; this.stage = "prepared"; }
}
function fullUpload(m, size) {
  for (let offset = 0; offset < size; offset += 1436)
    assert.equal(m.chunk(Math.min(1436, size - offset), offset), true);
}
let cases = 0;
const scenario = fn => { fn(); ++cases; };
scenario(() => {
  const m = new SimulatedZip();
  assert.ok(m.begin("spiffs", 1572864));
  fullUpload(m, 1572864);
  assert.ok(m.preverify(true)); assert.equal(m.flashTouched, false);
  assert.ok(m.ack()); assert.equal(m.ack(), false);
  assert.ok(m.flashResult(true, true));
  assert.ok(m.begin("firmware", 2850560));
  fullUpload(m, 2850560);
  assert.ok(m.preverify(true)); assert.equal(m.flashTouched, true);
  assert.ok(m.ack()); assert.ok(m.flashResult(true, true));
  assert.ok(m.commit()); assert.equal(m.psram, 0);
});
scenario(() => { // no ACK after HTTP 202: no Flash
  const m = new SimulatedZip(); assert.ok(m.begin("spiffs", 1572864));
  fullUpload(m,1572864); assert.ok(m.preverify(true));
  assert.equal(m.flashTouched,false); assert.equal(m.flashResult(true,true),false);
  m.abort(); assert.equal(m.psram,0); assert.equal(m.flashTouched,false);
});
scenario(() => { // SHA mismatch
  const m = new SimulatedZip(); assert.ok(m.begin("spiffs",1572864));
  fullUpload(m,1572864); assert.equal(m.preverify(false),false);
  assert.equal(m.flashTouched,false); m.abort(); assert.equal(m.psram,0);
});
scenario(() => { // fail closed if no 2 MiB reserve
  const m = new SimulatedZip(3 * MiB,3 * MiB);
  assert.equal(m.begin("firmware",2850560),false); assert.equal(m.flashTouched,false);
});
scenario(() => { // fragmented PSRAM
  const m = new SimulatedZip(6 * MiB, 1 * MiB);
  assert.equal(m.begin("spiffs",1572864),false);
});
scenario(() => { // incomplete image
  const m = new SimulatedZip(); assert.ok(m.begin("spiffs",1572864));
  assert.ok(m.chunk(1436,0)); assert.equal(m.preverify(true),false);
  assert.equal(m.flashTouched,false);
});
scenario(() => { // offset mismatch
  const m = new SimulatedZip(); assert.ok(m.begin("spiffs",1572864));
  assert.equal(m.chunk(1436,1436),false); assert.equal(m.flashTouched,false);
});
scenario(() => { // Flash write failure
  const m = new SimulatedZip(); assert.ok(m.begin("spiffs",1572864));
  fullUpload(m,1572864); assert.ok(m.preverify(true)); m.ack();
  assert.equal(m.flashResult(false,true),false); assert.equal(m.commit(),false);
  m.abort(); assert.equal(m.psram,0);
});
scenario(() => { // Flash readback SHA failure
  const m = new SimulatedZip(); assert.ok(m.begin("spiffs",1572864));
  fullUpload(m,1572864); assert.ok(m.preverify(true)); m.ack();
  assert.equal(m.flashResult(true,false),false); assert.equal(m.commit(),false);
});
scenario(() => { // cancel / retry without reboot
  const m = new SimulatedZip(); assert.ok(m.begin("spiffs",1572864));
  m.chunk(1436,0); m.abort(); assert.ok(m.begin("spiffs",1572864));
  assert.equal(m.psram,1572864);
});
scenario(() => { // both images at once are NOT allowed within reserve
  const total = 1572864 + 2850560;
  const m = new SimulatedZip();
  assert.equal(m.begin("firmware",total),false);
});
console.log("PASS: " + checks + " source contracts, " + cases +
            " fault-injected ZIP PSRAM/A-B state scenarios");

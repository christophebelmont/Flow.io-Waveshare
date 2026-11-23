#!/usr/bin/env node
/**
 * Experimental remote OTA source contract:
 * PSRAM first -> SHA-256 -> inactive A/B flash; fallback on memory pressure.
 * Run: node scripts/tests/check_ota_psram_preverify.mjs
 * The optional native C++ policy test does not replace a firmware build.
 */
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import os from "node:os";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = path => readFileSync(join(root, path), "utf8");
const ota = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const policy = read("src/Core/OtaPsramPolicy.h");
const web = read("data/webinterface/app.js");
const rtcH = read("src/Core/RebootDiagnostics.h");
const rtcCpp = read("src/Core/RebootDiagnostics.cpp");
const wifi = read("src/Modules/Network/WifiProvisioningModule/WifiProvisioningModule.h");
const release = read("src/Core/ReleaseStorage.cpp");

assert.match(ota, /#include "Core\/OtaPsramPolicy.h"/);
assert.match(ota, /OtaPsramPolicy::canStageImage\(\s*expectedSize,\s*ramFreeBefore,\s*ramLargestBefore\)/);
assert.match(policy, /2U \* 1024U \* 1024U/);
assert.match(policy, /freeBytes - kReserveBytes/);
assert.match(policy, /imageSize <= largestFreeBlock/);
assert.match(ota, /heap_caps_malloc\(expectedSize,\s*MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\)/);
assert.match(ota, /std::unique_ptr<uint8_t,\s*decltype\(&heap_caps_free\)> ramImage/);
assert.equal([...ota.matchAll(/uint8_t chunk\[1024\]\{\};/g)].length, 1,
  "share a single 1 KiB internal OTA stack buffer");

const start = ota.indexOf("// Experimental preverified PSRAM mode.");
const fallback = ota.indexOf('stage=fallback_stream', start);
const legacy = ota.indexOf('uint32_t maxFlashWriteUs = 0U;', fallback);
const commit = ota.indexOf("stage=commit_start", legacy);
assert.ok(start >= 0 && fallback > start && legacy > fallback && commit > legacy);
const ram = ota.slice(start, fallback);
function pos(snippet) {
  const index = ram.indexOf(snippet);
  assert.ok(index >= 0, "Missing stage: " + snippet);
  return index;
}
assert.ok(pos("ramHttp.end();") < pos("beginLocalImage_(transactionId, target, expectedSize"));
assert.ok(pos("sha256Matches_(digest, expectedSha)") < pos("beginLocalImage_(transactionId, target, expectedSize"));
assert.ok(pos('stage=preverify_end') < pos("beginLocalImage_(transactionId, target, expectedSize"));
assert.ok(pos("memcpy(chunk, ramImage.get() + flashed, amount)") <
          pos("writeLocalImage_(transactionId, target, chunk, amount"));
assert.ok(pos("writeLocalImage_(transactionId, target, chunk, amount") <
          pos("endLocalImage_(transactionId, target, errOut, errOutLen)"));
assert.match(ram, /if \(!shaOk\) \{\s*success = writeSimpleError_/);
assert.match(ram, /if \(success\) \{\s*\/\/ Only after complete HTTP/);
assert.match(ram, /RebootDiagnostics::OtaStage::SpiffsRamDownload/);
assert.match(ram, /RebootDiagnostics::OtaStage::FirmwareRamVerify/);
assert.match(ram, /stage=interrupted/);
assert.match(ram, /stage=image_end mode=psram/);
assert.match(ota.slice(fallback, commit), /uint32_t maxFlashWriteUs = 0U/);
assert.match(ota.slice(fallback, commit), /writeLocalImage_\(transactionId, target, chunk, \(size_t\)rd/);
assert.match(ota.slice(fallback, commit), /endLocalImage_\(transactionId, target, errOut, errOutLen\)/);
assert.match(ota, /for \(size_t imageIndex = 0U; success && imageIndex < 2U; \+\+imageIndex\)/);
assert.match(ota, /if \(!success && transactionId != 0U\) \{[\s\S]*?abortLocalRelease_\(/);
assert.match(ota, /commitLocalRelease_\(transactionId, errOut, errOutLen\)/);
assert.match(ota, /failLocalRelease_\("release target overlaps active partition"\)/);
assert.match(ota, /esp_ota_set_boot_partition\(target\)/);
assert.match(release, /return gRunningSlot == ReleaseSlot::A \? ReleaseSlot::B : ReleaseSlot::A/);
assert.match(web, /msg\.includes\('downloading firmware to psram'\)/);
assert.match(web, /msg\.includes\('sha-256 filesystem in psram'\)/);
assert.match(ota, /Keep the overall percentage tied to bytes committed to A\/B/);
assert.match(wifi, /if \(hadUsableNetwork_ && wifiEnabled_ && wifiConfigured_\) \{\s*return NetworkPortalReason::None;/);
assert.match(rtcH, /FirmwareRamVerify = 10/);
assert.match(rtcCpp, /case OtaStage::SpiffsRamDownload: return "spiffs_ram_download"/);

const compiler = process.env.CXX || "c++";
const temp = mkdtempSync(join(os.tmpdir(), "flow-psram-"));
try {
  const cpp = join(temp, "policy.cpp");
  const exe = join(temp, process.platform === "win32" ? "policy.exe" : "policy");
  const harness = [
    "#include <cassert>",
    "#include <cstddef>",
    '#include "Core/OtaPsramPolicy.h"',
    "int main() {",
    "  using OtaPsramPolicy::canStageImage;",
    "  constexpr size_t MiB = 1024U * 1024U;",
    "  assert(canStageImage(2827728U, 5700000U, 5630000U));",
    "  assert(canStageImage(1572864U, 5700000U, 5630000U));",
    "  assert(!canStageImage(0U, 5700000U, 5630000U));",
    "  assert(!canStageImage(2827728U, 4U * MiB, 4U * MiB));",
    "  assert(!canStageImage(2827728U, 6U * MiB, 2U * MiB));",
    "  assert(canStageImage(2U * MiB, 4U * MiB, 2U * MiB));",
    "  assert(!canStageImage(2U * MiB, 4U * MiB - 1U, 2U * MiB));",
    "}"
  ].join("\n");
  writeFileSync(cpp, harness);
  const build = spawnSync(compiler, ["-std=c++17", "-Wall", "-Wextra", "-Werror",
    "-I" + join(root, "src"), cpp, "-o", exe], { encoding: "utf8" });
  if (build.error && build.error.code === "ENOENT") {
    console.warn("SKIP: c++ compiler not installed; static checks passed");
  } else {
    assert.equal(build.status, 0, build.stderr || build.error?.message);
    const run = spawnSync(exe, [], { encoding: "utf8" });
    assert.equal(run.status, 0, run.stderr || run.error?.message);
    console.log("PASS: compiled PSRAM reserve policy boundary cases");
  }
} finally {
  rmSync(temp, { recursive: true, force: true });
}
console.log("PASS: PSRAM-first preverification, safe internal writes, streaming fallback, A/B, RTC, Wi-Fi AP fix");

#!/usr/bin/env node
/**
 * HTTP OTA retry regression (transport-policy and source-contract).
 * Run: node scripts/tests/check_ota_http_retry.mjs
 * A native host C++ compiler checks the production policy when available.
 * This is NOT a PlatformIO/ESP32 Wi-Fi integration test.
 */
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import os from "node:os";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = filename => readFileSync(join(root, filename), "utf8");
const ota = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const policy = read("src/Core/OtaHttpRetryPolicy.h");
const ramStart = ota.indexOf("// Experimental preverified PSRAM mode.");
const streamStart = ota.indexOf("stage=fallback_stream", ramStart);
const end = ota.indexOf("stage=commit_start", streamStart);
assert.ok(ramStart >= 0 && streamStart > ramStart && end > streamStart);
const ram = ota.slice(ramStart, streamStart);
const stream = ota.slice(streamStart, end);
const common = [/OtaHttpRetryPolicy::kMaxAttempts/,
  /const size_t requestedOffset = received;/,
  /if \(requestedOffset > 0U\)/,
  /addHeader\("Range", String\("bytes="\)/,
  /collectHeaders\(responseHeaders, 1U\)/,
  /header\("Content-Range"\)/,
  /OtaHttpRetryPolicy::responseMode\(/,
  /OtaHttpRetryPolicy::isRetryableStatus\(code\)/,
  /attempt == OtaHttpRetryPolicy::kMaxAttempts/,
  /stage=retries_exhausted/,
  /stage=retry_scheduled/,
  /if \(!success\) break;/
];
for (const [name, source] of [["RAM", ram], ["stream", stream]]) {
  for (const re of common) assert.match(source, re, name + " missing " + re);
  assert.match(source, /OtaHttpRetryPolicy::kRetryDelayMs/, "bounded delay in " + name);
  assert.match(source, /if \(mode == OtaHttpRetryPolicy::ResponseMode::Invalid\)/);
  assert.match(source, /HTTPClient (?:ramHttp|artifactHttp);/);
  assert.match(source, /(?:ramHttp|artifactHttp)\.end\(\);/, "close HTTP before retry");
}
assert.match(ram, /stage=restart_full server_no_range=1/);
assert.match(ram, /received = 0U;\s*lastLogged = 0U;/);
assert.match(ram, /sha256Matches_\(digest, expectedSha\)/);
assert.ok(ram.indexOf("sha256Matches_(digest, expectedSha)") <
          ram.indexOf("beginLocalImage_(transactionId, target, expectedSize"),
          "the RAM mode must verify SHA before writing any flash");
assert.match(stream, /stage=restart_full_skip bytes=/);
assert.match(stream, /replaySkip = requestedOffset;/);
assert.match(stream, /if \(replaySkip > 0U\) \{\s*replaySkip -= \(size_t\)rd;\s*vTaskDelay\(1\);\s*continue;/);
assert.match(stream, /success = writeLocalImage_\(transactionId, target, chunk/);
assert.match(stream, /endLocalImage_\(transactionId, target, errOut, errOutLen\)/);
assert.match(ota, /if \(!success && transactionId != 0U\) \{[\s\S]*?abortLocalRelease_\(/);
assert.match(policy, /kMaxAttempts = 3U/);
assert.match(policy, /status == 408 \|\| status == 429/);

// Compile actual production HTTP policy with a host compiler.
const compiler = process.env.CXX || "c++";
const temp = mkdtempSync(join(os.tmpdir(), "flow-ota-http-"));
try {
  const cpp = join(temp, "test.cpp");
  const exe = join(temp, process.platform === "win32" ? "test.exe" : "test");
  writeFileSync(cpp, [
    "#include <cassert>",
    '#include "Core/OtaHttpRetryPolicy.h"',
    "int main() {",
    "  using namespace OtaHttpRetryPolicy;",
    "  assert(kMaxAttempts == 3U);",
    "  assert(responseMode(200, 0, 1000, 1000, \"\") == ResponseMode::Full);",
    "  assert(responseMode(200, 400, 1000, 1000, \"\") == ResponseMode::Full);",
    "  assert(responseMode(206, 400, 600, 1000, \"bytes 400-999/1000\") == ResponseMode::Partial);",
    "  assert(responseMode(206, 400, 600, 1000, \"bytes 399-999/1000\") == ResponseMode::Invalid);",
    "  assert(responseMode(206, 400, 600, 1000, \"bytes 400-998/1000\") == ResponseMode::Invalid);",
    "  assert(responseMode(206, 400, 600, 1000, \"bytes 400-999/1001\") == ResponseMode::Invalid);",
    "  assert(responseMode(206, 400, 601, 1000, \"bytes 400-999/1000\") == ResponseMode::Invalid);",
    "  assert(responseMode(206, 0, 1000, 1000, \"bytes 0-999/1000\") == ResponseMode::Invalid);",
    "  assert(responseMode(206, 400, 600, 1000, \"bytes 400-999/1000 garbage\") == ResponseMode::Invalid);",
    "  assert(responseMode(200, 0, 999, 1000, \"\") == ResponseMode::Invalid);",
    "  assert(responseMode(404, 0, 1000, 1000, \"\") == ResponseMode::Invalid);",
    "  assert(isRetryableStatus(-1) && isRetryableStatus(-11));",
    "  assert(isRetryableStatus(408) && isRetryableStatus(429) && isRetryableStatus(500) && isRetryableStatus(503));",
    "  assert(!isRetryableStatus(200) && !isRetryableStatus(206) && !isRetryableStatus(403) && !isRetryableStatus(404) && !isRetryableStatus(416));",
    "  assert(!isRetryableStatus(400) && !isRetryableStatus(600));",
    "  return 0;",
    "}"
  ].join("\n"), "utf8");
  const built = spawnSync(compiler, ["-std=c++17", "-Wall", "-Wextra", "-Werror",
    "-I" + join(root, "src"), cpp, "-o", exe], { encoding: "utf8" });
  if (built.error?.code === "ENOENT") {
    console.warn("SKIP: C++ host compiler unavailable");
  } else {
    assert.equal(built.status, 0, built.stderr || built.error?.message);
    const run = spawnSync(exe, [], { encoding: "utf8" });
    assert.equal(run.status, 0, run.stderr || run.error?.message);
    console.log("PASS: compiled HTTP 200/206/Range/retryable status policy");
  }
} finally {
  rmSync(temp, { recursive: true, force: true });
}
console.log("PASS: PSRAM + streaming retries share 3-attempt cap, strict Range and safe restart");

#!/usr/bin/env node
/**
 * Ensure critical OTA and boot diagnostics survive LogEntry.msg formatting.
 * LOG_MSG_MAX is 160 (159 printable characters). A longer message used to
 * export "stack_free=4" instead of "stack_free=4xxx", masking healthy stack.
 *
 * Run: node scripts/tests/check_ota_log_line_lengths.mjs
 * Static contract, not a hardware or PlatformIO validation.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const root = new URL("../../", import.meta.url);
const read = path => readFileSync(new URL(path, root), "utf8");
const logger = read("src/Core/Services/ILogger.h");
const ota = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const boot = read("src/Core/RebootDiagnostics.cpp");
const max = Number(logger.match(/constexpr int LOG_MSG_MAX = (\d+);/)?.[1]);
assert.ok(Number.isInteger(max) && max >= 128, "Unknown LogEntry.msg bound");

const otaSection = ota.split("static void otaMonitor_(")[1]?.split("bool cloneActiveReleaseFilesystem_(")[0];
assert.ok(otaSection, "otaMonitor_() not found");
const otaFmts = [...otaSection.matchAll(/LOGI\("([^"]+)"/g)].map(x => x[1]);
assert.equal(otaFmts.length, 2, "Expected two bounded OTA diagnostics entries");
assert.match(otaFmts[0], /^\[OTA-MEM\] phase=%s task=%s id=%p core=%d hwm_bytes=%lu/);
assert.match(otaFmts[1], /^\[OTA-HEAP\] phase=%s ms=%lu min_internal=%lu/);
assert.doesNotMatch(otaFmts.join(" "), /stack_free=|stack_hwm=/, "Legacy truncated line left behind");

const bootFmts = [...boot.matchAll(/Log::info\(module,\s*"([^"]+)"/g)].map(x => x[1]);
assert.equal(bootFmts.length, 2, "Boot diagnostics must reach Web LogHub in two parts");
assert.match(bootFmts[0], /source=%s planned=%s/);
assert.match(bootFmts[1], /last_ota_op=%lu stage=%s received=%lu\/\%lu/);

// Worst case: 32-character phase, 16-character FreeRTOS task name,
// 32-bit pointer, negative signed core, and maximum 32-bit decimal fields.
// For boot records, reserve 20 chars for any named enumeration value.
function pessimisticLength(fmt) {
  let stringIndex = 0;
  const expanded = fmt.replace(/%(lu|u|d|p|s)/g, (spec) => {
    if (spec === "%s") {
      stringIndex++;
      if (fmt.includes("phase=")) return stringIndex === 1 ? "p".repeat(32) : "t".repeat(16);
      return "n".repeat(20);
    }
    if (spec === "%p") return "0xffffffff";
    if (spec === "%d") return "-1";
    return "4294967295";
  });
  return expanded.length;
}
for (const fmt of [...otaFmts, ...bootFmts]) {
  const length = pessimisticLength(fmt);
  assert.ok(length < max,
    `Log line can be truncated: worst=${length}, limit=${max - 1}: ${fmt}`);
}
assert.match(boot, /Serial\.printf\("\[BOOT-RESET\] reset_reason=/,
  "Keep direct serial diagnostics during early boot");
console.log(`PASS: OTA/boot diagnostics fit LogEntry.msg (${max - 1} characters)`);

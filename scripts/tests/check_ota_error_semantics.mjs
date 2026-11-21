#!/usr/bin/env node
/**
 * Regression check for 2026-11-21 OTA download stall:
 * 365991/1572864 bytes were received, yet the prior firmware reported
 * http_end success=1 because writeSimpleError_ returned true after snprintf.
 *
 * Run: node scripts/tests/check_ota_error_semantics.mjs
 * Static regression; firmware compilation and bench validation remain required.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const text = readFileSync(new URL("../../src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp", import.meta.url), "utf8");

const helperStart = text.indexOf("static bool writeSimpleError_(char* out, size_t outLen, const char* msg)\n{");
const helperEnd = text.indexOf("\n}\n", helperStart);
assert.ok(helperStart >= 0 && helperEnd > helperStart, "error helper definition not found");
const helper = text.slice(helperStart, helperEnd + 2);
assert.match(helper, /snprintf\(out, outLen, "%s", msg \? msg : "failed"\)/);
assert.match(helper, /\breturn false;\s*\}$/);
assert.doesNotMatch(helper, /return\s+n\s*>/);

const pairedStart = text.indexOf("bool FirmwareUpdateModule::runWaveshareUpdate_");
assert.ok(pairedStart >= 0, "paired update routine not found");
const pairedEnd = text.indexOf("\nbool FirmwareUpdateModule::runNextionUpdate_", pairedStart);
assert.ok(pairedEnd > pairedStart);
const paired = text.slice(pairedStart, pairedEnd);
const errorAssignments = [...paired.matchAll(/success\s*=\s*writeSimpleError_\(/g)];
assert.ok(errorAssignments.length >= 6,
  "expected original failure paths to keep using writeSimpleError_()");
assert.match(paired, /while \(success && !retryableFailure && received < expectedSize\)/);
assert.match(paired, /for \(size_t imageIndex = 0U; success && imageIndex < 2U; \+\+imageIndex\)/);
assert.match(paired, /stage=interrupted received=/);
assert.match(paired, /if \(!success && transactionId != 0U\) \{[\s\S]*?abortLocalRelease_\(/);
assert.match(paired, /stage=http_end success=%u/);

// The value returned after formatting a nonempty error string is a
// FAILURE, not an indication that snprintf happened to succeed.
const equivalent = (output, message) => {
  if (output) output.value = message || "failed";
  return false;
};
const out = {};
assert.equal(equivalent(out, "release download interrupted"), false);
assert.equal(out.value, "release download interrupted");
assert.equal(equivalent(null, "release download interrupted"), false);
console.log("PASS: OTA failure branches remain failures after HTTP retry changes");

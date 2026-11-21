#!/usr/bin/env node
/**
 * Host source-level guard for intermittent OTA crash diagnostics.
 * Run: node scripts/tests/check_ota_reboot_diags.mjs
 * This does NOT compile the ESP32 firmware or emulate RTC hardware.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const root = new URL("../../", import.meta.url);
const read = (path) => readFileSync(new URL(path, root), "utf8");
const reboot = read("src/Core/RebootDiagnostics.cpp");
const rebootHeader = read("src/Core/RebootDiagnostics.h");
const updater = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const system = read("src/Modules/System/SystemModule/SystemModule.cpp");
const network = read("src/Modules/Network/ModbusTcpServerModule/ModbusTcpServerModule.h");

assert.match(reboot, /RTC_NOINIT_ATTR\s+volatile\s+Record/);
assert.match(reboot, /record\.checksum\s*!=\s*checksumOf_\(record\)/);
assert.match(reboot, /rtcRecord\.magic = 0U; \/* One-shot/);
assert.match(reboot, /\[BOOT-RESET\].*planned=.*last_ota_op=.*received=/);
assert.match(rebootHeader, /Lost on power removal/);
assert.doesNotMatch(reboot, /nvs_set|Preferences|esp_partition_write|flash_write/);

assert.match(updater, /RebootDiagnostics::reportPreviousBoot\(\)/);
assert.match(updater, /RebootDiagnostics::noteOta\(job\.operationId,\s*RebootDiagnostics::OtaStage::Manifest/);
assert.match(updater, /RebootDiagnostics::OtaStage::Spiffs/);
assert.match(updater, /RebootDiagnostics::OtaStage::Firmware/);
assert.match(updater, /RebootDiagnostics::OtaStage::VerifyFirmware/);
assert.match(updater, /RebootDiagnostics::OtaStage::Commit/);
assert.match(updater, /RebootDiagnostics::markPlanned\(RebootDiagnostics::Origin::OtaNextion, job\.operationId\);\s*ESP\.restart\(\)/);
assert.match(updater, /RebootDiagnostics::markPlanned\(RebootDiagnostics::Origin::OtaRelease,\s*localRelease_\.operationId\);\s*ESP\.restart\(\)/);
assert.match(system, /RebootDiagnostics::markPlanned\([\s\S]*?RebootDiagnostics::Origin::System\);\s*esp_restart\(\)/);

// This is the regression behind the missing OTA-NET traces: an early return
// used to suppress all diagnostics when no TCP port or station IP was active.
const loop = network.split("void loop() override {")[1]?.split("    // MQTT JSON extension")[0];
assert.ok(loop);
assert.ok(loop.indexOf("const bool otaBusy") < loop.indexOf("if (!eligible_) return;"));
assert.ok(loop.indexOf("[OTA-NET] ms=") < loop.indexOf("if (!eligible_) return;"));
assert.match(loop, /station=%u listening=%u eligible=%u/);
assert.match(loop, /if \(otaBusy\) \{\s*if \(elapsedUs > otaTickMaxUs_\)/);
assert.match(loop, /if \(uint32_t\(now - lastOtaTraceMs_\) >= 5000U\)/);
assert.match(network, /taskStackSize\(\) const override \{ return 4096; \}/);
assert.doesNotMatch(reboot, /malloc|new\s+|heap_caps_malloc/);

console.log("PASS: OTA reboot breadcrumbs, planned reset marks, offline TCP diagnostics, and untouched task stack");

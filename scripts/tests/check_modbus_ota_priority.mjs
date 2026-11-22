#!/usr/bin/env node
/**
 * OTA-first gateway policy. Source contract plus timing boundary checks.
 * Does not replace the ESP32 PlatformIO build or on-device measurements.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const root = new URL("../../", import.meta.url);
const read = path => readFileSync(new URL(path, root), "utf8");
const m = read("src/Modules/Network/ModbusTcpServerModule/ModbusTcpServerModule.h");
const p = read("src/Modules/Network/ModbusTcpServerModule/ModbusTcpServerPolicy.h");
const s = read("src/Modules/Network/ModbusTcpServerModule/ModbusTcpSocketServer.h");
const c = read("src/Modules/Network/ModbusTcpServerModule/ModbusTcpServerModule.cpp");
const ota = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const logger = read("src/Core/Services/ILogger.h");

const loop = m.split("void loop() override {")[1]?.split("// MQTT JSON extension")[0];
assert.ok(loop, "Modbus loop missing");
assert.match(m, /taskStackSize\(\) const override \{ return 5120; \}/);
assert.doesNotMatch(loop, /char otaNetLine\[384\]/);
assert.match(loop, /if \(otaBusy && uint32_t\(now - otaBusySinceMs_\) >= 300U\)/);
assert.match(loop, /if \(!otaQuiesced_\) \{/);
assert.match(loop, /server_\.stop\(\);\s*running_ = false;/);
assert.match(loop, /\[MODBUS-TCP\] ota_priority=1 paused=1 pool=/);
assert.match(loop, /\[MODBUS-TCP\] ota_priority=0 resume=1 pool=/);
assert.ok(loop.indexOf("if (otaBusy && uint32_t(now - otaBusySinceMs_) >= 300U)") <
          loop.indexOf("server_.tick(now)"),
          "OTA quiesce must precede all Modbus work");
assert.ok(loop.indexOf("if (otaBusy && uint32_t(now - otaBusySinceMs_) >= 300U)") <
          loop.indexOf("if (!running_)"),
          "OTA quiesce must prevent accepting/restarting TCP listeners");
assert.ok(loop.includes("logWebSnapshot_(now, stationConnected, true)"),
          "OTA pressure samples must remain available without a listener");
assert.match(m, /uint32_t otaBusySinceMs_ = 0U/);
assert.match(m, /bool otaQuiesced_ = false/);
assert.match(s, /void stop\(\) \{[\s\S]*?close_\(slot\)/);
assert.match(s, /bridges_\[slot\]\.reset\(\);/);
assert.match(s, /FrameStorage\* frameStorage_ = nullptr;/);
assert.match(s, /lastResolverUnitId = request\.unitId/);
assert.match(s, /lastResolverFunction = request\.function/);
assert.match(s, /lastResolverAddress = request\.address/);
assert.match(p, /uint8_t lastResolverUnitId = 0U;/);
assert.match(c, /\[MODBUS-RESOLVE\] port=%u mode=%u device=%u unit=%u fc=%u addr=%u fails=%lu age_ms=%lu/);
const worst = "[MODBUS-RESOLVE] port=%u mode=%u device=%u unit=%u fc=%u addr=%u fails=%lu age_ms=%lu"
  .replace(/%lu/g, "4294967295").replace(/%u/g, "65535").length;
const bound = Number(logger.match(/constexpr int LOG_MSG_MAX = (\d+);/)?.[1]);
assert.ok(worst < bound, "Resolver summary truncated in Web logs: " + worst);
// Keep the proven OTA mechanism untouched: completed PSRAM verify then
// firmware erase using esp_ota_begin, target inactive and A/B commit.
assert.match(ota, /flashError = esp_ota_begin\(localRelease_\.writePartition, totalSize, &localRelease_\.otaHandle\);/);
assert.match(ota, /stage=preverify_end sha256=%s ms=%lu flash_touched=0/);
assert.match(ota, /RebootDiagnostics::markPlanned\(RebootDiagnostics::Origin::OtaRelease,/);
// Same unsigned arithmetic works across a millis() rollover.
const passed = (now, since) => ((now - since) >>> 0) >= 300;
assert.equal(passed(299, 0), false);
assert.equal(passed(300, 0), true);
assert.equal(passed(0x0000002c, 0xfffffff0), false);
assert.equal(passed(0x00000130, 0xfffffff0), true);
console.log("PASS: OTA-first Modbus quiesce, safe stack budget, resolver metadata, unchanged OTA writer");

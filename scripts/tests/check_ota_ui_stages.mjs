#!/usr/bin/env node
/**
 * Source-level regression for the paired OTA A/B progress indicator.
 * Run: node scripts/tests/check_ota_ui_stages.mjs
 * Does not replace ESP32-S3 firmware compilation or a browser run.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const root = new URL("../../", import.meta.url);
const read = (path) => readFileSync(new URL(path, root), "utf8");
const app = read("data/webinterface/app.js");
const shell = read("data/webinterface/sh.html");
const css = read("data/webinterface/app-core.css");
const updater = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const fr = JSON.parse(read("data/webinterface/i18n/fr.json")).translations;
const en = JSON.parse(read("data/webinterface/i18n/en.json")).translations;

assert.match(shell, /id="upgradeStageDetail"/);
assert.match(css, /\.updates-progress-stage\{/);
assert.equal([...app.matchAll(/\{ id: 'ota-\d+'/g)].length, 15);
assert.equal([...app.matchAll(/\{ id: 'zip-\d+'/g)].length, 15);
for (const key of Object.keys(fr).filter(k => k.startsWith("updates.zipStage"))) {
  assert.equal(typeof en[key], "string", "missing ZIP English label: " + key);
}
assert.equal(Object.keys(fr).filter(k => k.startsWith("updates.zipStage")).length, 15);
for (const key of Object.keys(fr).filter(k => k.startsWith("updates.otaStage"))) {
  assert.equal(typeof en[key], "string", "missing English translation: " + key);
}
assert.match(app, /upgradeStageDetail\.textContent = stageText/);
assert.match(app, /backendMessage: msg/);

// Evaluate only the two pure stage-mapping helper functions.
const paired = app.match(/function upgradeIsPairedTarget\(target\) \{[\s\S]*?\n    \}/);
const stage = app.match(/function upgradePairedOtaStageIndex\(session\) \{[\s\S]*?\n    \}/);
assert.ok(paired && stage, "missing paired OTA stage functions");
const map = new Function(paired[0] + "\n" + stage[0] + "\nreturn upgradePairedOtaStageIndex;")();
const samples = [
  ["target", "", 0],
  ["download", "downloading paired release manifest", 1],
  ["flash", "preparing release filesystem", 4],
  ["flash", "writing release filesystem", 5],
  ["flash", "verifying release filesystem", 6],
  ["flash", "filesystem SHA-256 OK; firmware next", 7],
  ["flash", "preparing release firmware", 9],
  ["flash", "writing release firmware", 10],
  ["flash", "verifying release firmware", 11],
  ["flash", "firmware SHA-256 OK; committing release", 12],
  ["flash", "activating paired release", 12],
  ["reboot", "", 13],
  ["reconnect", "", 14],
  ["done", "", 14],
  ["flash", "unknown old-firmware message", null]
];
for (const [phase, backendMessage, expected] of samples) {
  assert.equal(map({ target: "waveshare", phase, backendMessage }), expected, phase + " " + backendMessage);
}
for (const message of [
  "preparing release filesystem", "verifying release filesystem",
  "preparing release firmware", "verifying release firmware", "activating paired release"
]) {
  assert.ok(updater.includes(message), "backend stage is missing: " + message);
}
const zipStages = [
  ["zip_prepare",0], ["zip_manifest",1],
  ["zip_spiffs_receive",2], ["zip_spiffs_ram_verify",3],
  ["zip_spiffs_prepare",4], ["zip_spiffs_flash",5],
  ["zip_spiffs_readback",6], ["zip_spiffs_verified",6],
  ["zip_firmware_receive",7], ["zip_firmware_ram_verify",8],
  ["zip_firmware_prepare",9], ["zip_firmware_flash",10],
  ["zip_firmware_readback",11], ["zip_firmware_verified",11],
  ["zip_activate",12]
];
for (const [otaBackendStage,index] of zipStages) {
  assert.equal(map({target:"release",phase:"flash",otaBackendStage}),index,otaBackendStage);
}
assert.equal(map({target:"release",phase:"reboot"}),13);
assert.equal(map({target:"release",phase:"reconnect"}),14);
assert.equal(map({ target: "nextion", phase: "flash", backendMessage: "" }), null,
  "Nextion must retain its existing generic progress UI");
console.log("PASS: 15 remote OTA + 15 local ZIP bilingual stages; Nextion unchanged");

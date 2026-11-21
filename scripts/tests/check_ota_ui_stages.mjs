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
assert.equal([...app.matchAll(/\{ id: 'ota-\d+'/g)].length, 11);
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
  ["flash", "preparing release filesystem", 2],
  ["flash", "writing release filesystem", 3],
  ["flash", "verifying release filesystem", 4],
  ["flash", "filesystem SHA-256 OK; firmware next", 5],
  ["flash", "preparing release firmware", 5],
  ["flash", "writing release firmware", 6],
  ["flash", "verifying release firmware", 7],
  ["flash", "firmware SHA-256 OK; committing release", 8],
  ["flash", "activating paired release", 8],
  ["reboot", "", 9],
  ["reconnect", "", 10],
  ["done", "", 10],
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
assert.equal(map({ target: "nextion", phase: "flash", backendMessage: "" }), null,
  "Nextion must retain its existing generic progress UI");
console.log("PASS: paired OTA has 11 localized steps, 15 stage mappings, and matching backend stages");

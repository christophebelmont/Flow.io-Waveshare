#!/usr/bin/env node
/**
 * Paired OTA UI stage regression, no browser or ESP32 required.
 * Run: node scripts/tests/check_ota_ui_phases.mjs
 * Executes the stage/percent mapping extracted from the real app.js.
 * Source contracts also check that the real firmware emits stage telemetry.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const load = path => readFileSync(join(root, path), "utf8");
const js = load("data/webinterface/app.js");
const cpp = load("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const header = load("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.h");
const server = load("src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp");
const names = [
  "upgradeIsPairedTarget",
  "upgradeStepDefinitions",
  "upgradePairedOtaStageIndex",
  "upgradePairedPhaseDetail",
  "upgradePhasePercent",
  "upgradePhaseTitle"
];
function extract(name) {
  const anchor = "    function " + name + "(";
  const i = js.indexOf(anchor);
  const j = js.indexOf("\n    function ", i + anchor.length);
  assert.ok(i >= 0 && j > i, "source function missing: " + name);
  assert.equal(js.indexOf(anchor, i + 1), -1, "duplicate function: " + name);
  return js.slice(i, j);
}
// Syntax-compile the entire real browser script as well as its pure helpers.
new Function(js);
const api = new Function("tr",
  names.map(extract).join("\n") + "\nreturn {" + names.join(",") + "};"
)((key, fallback) => fallback);
assert.equal(api.upgradeStepDefinitions("waveshare").length, 15);
assert.equal(api.upgradeStepDefinitions("nextion").length, 5);
const sample = (stage, phase, pct = 0) => ({
  target: "waveshare", phase, otaBackendStage: stage,
  otaBackendStepProgress: pct, backendMessage: ""
});
const cases = [
  ["manifest", "download", 1],
  ["spiffs_download", "download", 2],
  ["spiffs_ram_verify", "flash", 3],
  ["spiffs_prepare", "flash", 4],
  ["spiffs_write", "flash", 5],
  ["spiffs_stream", "flash", 5],
  ["spiffs_verify", "flash", 6],
  ["firmware_download", "download", 7],
  ["firmware_ram_verify", "flash", 8],
  ["firmware_prepare", "flash", 9],
  ["firmware_write", "flash", 10],
  ["firmware_stream", "flash", 10],
  ["firmware_verify", "flash", 11],
  ["activate", "flash", 12]
];
for (const [stage, phase, expected] of cases) {
  const current = sample(stage, phase, 55);
  assert.equal(api.upgradePairedOtaStageIndex(current), expected, stage);
  assert.ok(api.upgradePhaseTitle(current).length > 5, stage + " title");
  assert.equal(api.upgradePairedPhaseDetail(current.target, phase, stage, 55, "").length > 5,
    true, stage + " descriptive detail");
}
assert.equal(api.upgradePairedOtaStageIndex(sample("", "reboot")), 13);
assert.equal(api.upgradePairedOtaStageIndex(sample("", "reconnect")), 14);
assert.match(api.upgradePairedPhaseDetail("waveshare", "flash", "spiffs_stream", 42, ""),
  /simultanées/);
assert.match(api.upgradePairedPhaseDetail("waveshare", "download", "spiffs_download", 42, ""),
  /42%/);
assert.match(api.upgradePairedPhaseDetail("waveshare", "flash", "spiffs_write", 91, ""),
  /91%/);
assert.notEqual(api.upgradePhaseTitle(sample("spiffs_download", "download")),
  api.upgradePhaseTitle(sample("spiffs_write", "flash")),
  "download and flash must have different titles");
const fullOta = [
  sample("manifest", "download"),
  sample("spiffs_download", "download", 0),
  sample("spiffs_download", "download", 50),
  sample("spiffs_download", "download", 100),
  sample("spiffs_ram_verify", "flash"),
  sample("spiffs_prepare", "flash"),
  sample("spiffs_write", "flash", 0),
  sample("spiffs_write", "flash", 50),
  sample("spiffs_write", "flash", 100),
  sample("spiffs_verify", "flash"),
  sample("firmware_download", "download", 0),
  sample("firmware_download", "download", 70),
  sample("firmware_download", "download", 100),
  sample("firmware_ram_verify", "flash"),
  sample("firmware_prepare", "flash"),
  sample("firmware_write", "flash", 0),
  sample("firmware_write", "flash", 75),
  sample("firmware_write", "flash", 100),
  sample("firmware_verify", "flash"),
  sample("activate", "flash"),
  sample("", "reboot"),
  sample("", "reconnect")
];
const overall = fullOta.map(s => api.upgradePhasePercent(s));
assert.ok(overall.every((n, i) => i === 0 || n >= overall[i - 1]), "overall bar must not regress");
assert.ok(overall[2] > overall[1], "download bytes must move overall bar");
assert.ok(overall[7] > overall[6], "flash write bytes must move overall bar");
assert.match(js, /next\.lastPercent = Math\.max\(/,
  "restarting an HTTP 200 download may reset phase%, not global%");
const legacy = { target: "waveshare", phase: "flash", backendMessage: "writing release filesystem" };
assert.equal(api.upgradePairedOtaStageIndex(legacy), 5, "compatibility with older status JSON");

// Backend emits exact stage names, with per-image percent. ZIP path is unchanged.
assert.match(header, /char otaStage\[28\]/);
assert.match(header, /uint8_t otaStageProgress = 0U/);
assert.match(cpp, /doc\["ota_stage"\] = snap\.otaStage/);
assert.match(cpp, /doc\["ota_stage_progress"\] = snap\.otaStageProgress/);
assert.match(cpp, /if \(measureJson\(doc\) >= outLen && snap\.otaStage\[0\] !=/);
assert.match(server, /char out\[640\] = \{0\};/);
assert.match(cpp, /if \(downloadPercent != lastUiPercent\)/);
assert.match(cpp, /setOtaStage_\(nullptr, downloadPercent\)/);
assert.match(cpp, /setOtaStage_\(nullptr, 0U\)/);
assert.match(cpp, /setOtaStage_\("manifest", 0U\)/);
assert.match(cpp, /setOtaStage_\(imageIndex == 0U \? "spiffs_download" : "firmware_download"/);
assert.match(cpp, /setOtaStage_\(imageIndex == 0U \? "spiffs_write" : "firmware_write", 0U\)/);
assert.match(cpp, /setOtaStage_\(imageIndex == 0U \? "spiffs_stream" : "firmware_stream", 0U\)/);
assert.match(cpp, /setOtaStage_\(nullptr,\s*\(uint8_t\)\(\(\(uint64_t\)localRelease_\.received \* 100U\) \/ expectedSize\)\)/);
assert.match(cpp, /setStatus_\(UpdateState::Downloading, FirmwareUpdateTarget::Waveshare/);
assert.match(js, /otaBackendStage: otaBackendStage/);
assert.match(js, /otaBackendStepProgress: otaBackendStepProgress/);
for (const lang of ["en", "fr"]) {
  const { translations } = JSON.parse(load("data/webinterface/i18n/" + lang + ".json"));
  for (const stage of ["downloadSpiffs", "verifyRamSpiffs", "writeSpiffs",
    "verifySpiffs", "downloadFirmware", "verifyRamFirmware",
    "writeFirmware", "verifyFirmware", "streaming"]) {
    assert.ok(translations["updates.otaStage." + stage], lang + " missing " + stage);
  }
}

// Execute the REAL status-to-UI integration, not only pure label mappings.
const wiringSource = [
  "let upgradeUiStatusMuted = false;",
  "let session = { target: 'waveshare', operationId: 42, phase: 'target', awaitingReconnect: false };",
  "function readUpgradeUiSession() { return session; }",
  "function updateUpgradeUiSession(patch) { session = { ...session, ...patch }; return session; }",
  "function stopUpgradeReconnectFlow() {}",
  ...["upgradeIsPairedTarget", "upgradeStepDefinitions", "upgradePairedOtaStageIndex",
    "upgradePairedPhaseDetail", "updateUpgradeView"].map(extract),
  "return { send: data => { updateUpgradeView(data); return { ...session }; } };"
].join("\n");
const wire = new Function("tr", wiringSource)((key, fallback) => fallback);
function poll(state, stage, percentage) {
  return wire.send({
    ok: true, state: state, target: "waveshare", operation_id: 42,
    boot_id: 1, progress: 20, ota_stage: stage, ota_stage_progress: percentage, msg: "test"
  });
}
for (const [state, stage, pct, expected] of [
  ["downloading", "spiffs_download", 42, "Téléchargement SPIFFS en PSRAM · 42%"],
  ["flashing", "spiffs_write", 61, "Écriture du SPIFFS en flash · 61%"],
  ["downloading", "firmware_download", 17, "Téléchargement firmware en PSRAM · 17%"],
  ["flashing", "firmware_write", 91, "Écriture du firmware en flash · 91%"]
]) {
  const result = poll(state, stage, pct);
  assert.equal(result.detail, expected, "real polling UI phase: " + stage);
  assert.equal(result.otaBackendStage, stage);
  assert.equal(result.otaBackendStepProgress, pct);
}
assert.match(poll("flashing", "firmware_stream", 49).detail, /simultanées/);

console.log("PASS: 15 paired OTA stages, independent download/write percentages, monotonic UI, streaming fallback, FR/EN");

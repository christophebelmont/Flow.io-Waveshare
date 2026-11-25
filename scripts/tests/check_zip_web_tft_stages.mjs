#!/usr/bin/env node
/**
 * Source-level and executable pure-logic regression:
 * local ZIP (PSRAM receive/hash -> Flash write/readback -> A/B -> reboot).
 * Run: node scripts/tests/check_zip_web_tft_stages.mjs
 * Does not replace a hardware build, TFT photograph, or live browser test.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const base = new URL("../../", import.meta.url);
const read = path => readFileSync(new URL(path, base), "utf8");
const app = read("data/webinterface/app.js");
const fw = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const tft = read("src/Modules/TFTModuleS3/TFTModuleS3.cpp");
const stages = read("src/Modules/TFTModuleS3/TftUpdateStages.h");
const iface = read("src/Core/Services/IFirmwareUpdate.h");
const server = read("src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp");
const fr = JSON.parse(read("data/webinterface/i18n/fr.json")).translations;
const en = JSON.parse(read("data/webinterface/i18n/en.json")).translations;
const extract = name => {
  const start = app.indexOf("    function " + name + "(");
  const end = app.indexOf("\n    function ", start + 15);
  assert.ok(start >= 0 && end > start, "real browser function " + name);
  return app.slice(start, end);
};
const helperNames = [
  "upgradeIsPairedTarget", "upgradeIsZipTarget", "upgradeHasDetailedStages",
  "upgradeStepDefinitions", "upgradePairedOtaStageIndex", "upgradeStepState"
];
function load(lang) {
  const translation = lang === "en" ? en : fr;
  return new Function("tr", helperNames.map(extract).join("\n") +
    "\nreturn {" + helperNames.join(",") + "};")(
      (key, fallback) => translation[key] || fallback
    );
}
const apiFR = load("fr"), apiEN = load("en");
for (const [language, api] of [["fr",apiFR],["en",apiEN]]) {
  const zip = api.upgradeStepDefinitions("release");
  const remote = api.upgradeStepDefinitions("waveshare");
  const nextion = api.upgradeStepDefinitions("nextion");
  assert.equal(zip.length, 15, language + " ZIP steps");
  assert.equal(remote.length, 15, language + " remote OTA steps unchanged");
  assert.equal(nextion.length, 5, language + " Nextion steps unchanged");
  for (let i=0;i<15;i++) {
    assert.equal(zip[i].id,"zip-"+i);
    assert.ok(zip[i].label.length > 4,"translation ZIP "+i+" "+language);
    if (i > 0) assert.notEqual(zip[i].label,zip[i-1].label);
  }
  assert.ok(/SPIFFS/.test(zip[2].label));
  assert.ok(/SHA-256/.test(zip[3].label));
  assert.ok(/SHA-256/.test(zip[6].label));
  assert.ok(/SHA-256/.test(zip[11].label));
  assert.ok(/A\/B/.test(zip[12].label));
}
const journey = [
 ["zip_prepare",0],["zip_manifest",1],
 ["zip_spiffs_receive",2],["zip_spiffs_ram_verify",3],
 ["zip_spiffs_prepare",4],["zip_spiffs_flash",5],
 ["zip_spiffs_readback",6],["zip_spiffs_verified",6],
 ["zip_firmware_receive",7],["zip_firmware_ram_verify",8],
 ["zip_firmware_prepare",9],["zip_firmware_flash",10],
 ["zip_firmware_readback",11],["zip_firmware_verified",11],
 ["zip_activate",12]
];
for (const [phase,index] of journey) {
  const s = {target:"release",phase:"flash",otaBackendStage:phase,otaStageIndex:index};
  assert.equal(apiFR.upgradePairedOtaStageIndex(s), index, phase);
  assert.equal(apiFR.upgradeStepState("zip-"+index,s),"active",phase);
  if (index > 0) assert.equal(apiFR.upgradeStepState("zip-"+(index-1),s),"done");
  if (index < 14) assert.equal(apiFR.upgradeStepState("zip-"+(index+1),s),"pending");
  if (index > 1) assert.ok(fw.includes('"' + phase + '"') ||
     /_verified$/.test(phase), "backend emits ZIP step " + phase);
}
assert.equal(apiFR.upgradePairedOtaStageIndex({target:"release",phase:"reboot"}),13);
assert.equal(apiFR.upgradePairedOtaStageIndex({target:"release",phase:"reconnect"}),14);
assert.equal(apiFR.upgradeStepState("zip-14",{target:"release",phase:"done",otaStageIndex:14}),"done");
const ota = apiFR.upgradePairedOtaStageIndex({target:"waveshare",phase:"flash",otaBackendStage:"spiffs_write"});
assert.equal(ota,5,"remote OTA mapping preserved");
assert.equal(apiFR.upgradePairedOtaStageIndex({target:"nextion",phase:"flash"}),null);
const zipBlock = stages.slice(stages.indexOf("static constexpr Stage zip[]"),
                              stages.indexOf("static constexpr Stage hmi[]"));
const tftTokens = [...zipBlock.matchAll(/\{"(zip_[^"]+|reboot)", "([^"]+)", "([^"]+)"\}/g)];
assert.equal(tftTokens.length,14,"TFT 14 local stages (no reconnect on device)");
assert.equal(tftTokens[0][1],"zip_prepare");
assert.equal(tftTokens[13][1],"reboot");
for (const tok of tftTokens) {
  assert.ok(tok[2].length && tok[3].length);
  assert.ok(!/[^\x00-\x7F]/.test(tok[2]+tok[3]),"TFT GFX labels must be ASCII");
}
assert.ok(stages.includes('strcmp(phase, "zip_spiffs_verified")'));
assert.ok(stages.includes('strcmp(phase, "zip_firmware_verified")'));
assert.ok(tft.includes('updateLastWasZip_ = false;'));
assert.ok(tft.includes('updateLastWasZip_ = true;'));
assert.ok(tft.includes('zipUpdate ? "ZIP FIRMWARE + SPIFFS"'));
assert.ok(tft.includes('queued, rebooting, zipUpdate'));
assert.ok(fw.includes('setOtaStage_(fs ? "zip_spiffs_readback" : "zip_firmware_readback", 0U)'));
assert.ok(fw.includes('setOtaStage_(filesystemTarget ? "zip_spiffs_ram_verify"'));
assert.ok(fw.includes('setOtaStage_(filesystemTarget ? "zip_spiffs_prepare"'));
assert.ok(fw.includes('setOtaStage_("zip_manifest", 100U)'));
assert.ok(fw.includes('setOtaStage_("zip_activate", 0U)'));
assert.ok(iface.includes("Verifying, Verified, Error"));
assert.ok(server.includes('case FirmwareLocalImagePhase::Verifying: phase = "verifying"'));
assert.ok(app.includes("progress.phase === 'verifying'"));
assert.ok(app.includes("upgradeIsZipTarget(current && current.target)"));
assert.ok(app.includes("state === 'rebooting'"));
assert.ok(app.includes("await waitForZipImageFlash(transactionId, 'spiffs')"));
assert.ok(app.includes("await waitForZipImageFlash(transactionId, 'firmware')"));
console.log("PASS: 15 ZIP web steps + 14 TFT labels FR/EN; " +
  journey.length + " stage transitions, A/B, reboot, reconnect, remote OTA/Nextion intact");

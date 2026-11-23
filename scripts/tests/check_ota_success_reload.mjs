#!/usr/bin/env node
/**
 * Simulated browser regression for the OTA success refresh countdown.
 * Run: node scripts/tests/check_ota_success_reload.mjs
 * Does not replace a real-browser test with newly deployed SPIFFS assets.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const root = new URL("../../", import.meta.url);
const read = file => readFileSync(new URL(file, root), "utf8");
const app = read("data/webinterface/app.js");
const markup = read("data/webinterface/sh.html");
const css = read("data/webinterface/app-core.css");
const fr = JSON.parse(read("data/webinterface/i18n/fr.json")).translations;
const en = JSON.parse(read("data/webinterface/i18n/en.json")).translations;

assert.match(markup, /id="upgradeSuccessReloadCountdown"/);
assert.match(css, /\.updates-reload-countdown\{/);
assert.ok(fr["updates.reloadCountdown"].includes("{seconds}"));
assert.ok(en["updates.reloadCountdown"].includes("{seconds}"));
assert.match(app, /&& !shouldDeferWebAssetReloadForOta\(\)/);
assert.match(app, /if \(receiptResult === 'succeeded'\) \{[\s\S]*?scheduleUpgradeReloadAfterSuccess\(currentOperationId, completedTarget\)/);

const start = app.indexOf("    function stopUpgradeSuccessReloadCountdown() {");
const end = app.indexOf("    function markUpgradeUiCompletedAfterReceipt(", start);
assert.ok(start >= 0 && end > start);
const source = app.slice(start, end);
const el = { hidden: true, textContent: "" };
const storage = new Map();
let now = 0;
let nextId = 0;
let tasks = [];
let reloads = 0;
let session = { phase: "done", operationId: 101, target: "waveshare" };
const setTimer = (callback, delay) => {
  const id = ++nextId;
  tasks.push({ id, due: now + delay, callback });
  return id;
};
const clearTimer = id => { tasks = tasks.filter(task => task.id !== id); };
const flow = new Function("el", "storage", "getSession", "setTimer", "clearTimer", "reload", 
  'let upgradeSuccessReloadCountdownTimer = null;\n'
  + 'const upgradeSuccessReloadCountdown = el;\n'
  + 'const upgradeSuccessReloadStorageKey = "flow_upgrade_success_reloaded";\n'
  + 'const sessionStorage = storage;\n'
  + 'const readUpgradeUiSession = getSession;\n'
  + 'const setTimeout = setTimer;\n'
  + 'const clearTimeout = clearTimer;\n'
  + 'const window = {location:{reload}};\n'
  + 'const upgradeUsesReconnect = target => ["waveshare","spiffs","nextion","esp32s3","flowios3"].includes(target);\n'
  + 'const tr = (key, fallback) => fallback;\n'
  + source + '\nreturn {scheduleUpgradeReloadAfterSuccess, stopUpgradeSuccessReloadCountdown};'
)(el,
 { getItem: key => storage.get(key) || null, setItem: (key, value) => storage.set(key, value) },
 () => session, setTimer, clearTimer, () => { reloads++; });

function advance(ms) {
  now += ms;
  const due = tasks.filter(task => task.due <= now);
  tasks = tasks.filter(task => task.due > now);
  due.forEach(task => task.callback());
}

assert.equal(flow.scheduleUpgradeReloadAfterSuccess(101, "waveshare"), true);
assert.match(el.textContent, /3 s/);
assert.equal(el.hidden, false);
assert.equal(flow.scheduleUpgradeReloadAfterSuccess(101, "waveshare"), false);
assert.equal(tasks.length, 1);
advance(1000); assert.match(el.textContent, /2 s/);
advance(1000); assert.match(el.textContent, /1 s/);
assert.equal(reloads, 0);
advance(1000); assert.equal(reloads, 1);
assert.equal(el.hidden, true);
assert.equal(flow.scheduleUpgradeReloadAfterSuccess(101, "waveshare"), false);

session = { phase: "done", operationId: 102, target: "waveshare" };
assert.equal(flow.scheduleUpgradeReloadAfterSuccess(102, "waveshare"), true);
session = { phase: "flash", operationId: 103, target: "waveshare" };
advance(1000);
assert.equal(reloads, 1);
assert.equal(el.hidden, true);
assert.equal(tasks.length, 0);
assert.equal(flow.scheduleUpgradeReloadAfterSuccess(0, "waveshare"), false);
assert.equal(flow.scheduleUpgradeReloadAfterSuccess(103, "release"), false);

const guardStart = app.indexOf("    function shouldDeferWebAssetReloadForOta() {");
const guardEnd = app.indexOf("    async function loadWebMeta(", guardStart);
assert.ok(guardStart >= 0 && guardEnd > guardStart);
const guardSource = app.slice(guardStart, guardEnd);
const guard = new Function("getSession", "uses", "timer", 
  'const readUpgradeUiSession = getSession;\n'
  + 'const upgradeUsesReconnect = uses;\n'
  + 'const upgradeSuccessReloadCountdownTimer = timer;\n'
  + guardSource
  + '\nreturn shouldDeferWebAssetReloadForOta;'
)(() => session, target => target === "waveshare", null);
assert.equal(guard(), true); // A firmware transfer must not be auto-reloaded.
console.log("PASS: 3/2/1 countdown, single refresh, cancellation on new OTA, locale and asset reload guard");

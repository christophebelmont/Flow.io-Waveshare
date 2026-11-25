#!/usr/bin/env node
/**
 * Test the REAL waitForZipImageFlash() implementation from app.js with
 * simulated replies. Run: node scripts/tests/simulate_zip_browser_poll.mjs
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const js = readFileSync(new URL("../../data/webinterface/app.js", import.meta.url), "utf8");
const start = js.indexOf("async function waitForZipImageFlash(");
const end = js.indexOf("async function installLocalRelease(", start);
assert.ok(start > 0 && end > start, "browser wait-for-verified helper exists");
const code = js.slice(start, end);
const make = (fetch, consoleLog, messages) =>
  new Function("fetchOkJson", "setUpgradeDetail", "console", "setTimeout",
               code + "\nreturn waitForZipImageFlash;")(
    fetch, (msg) => messages.push(msg), consoleLog, (cb) => cb());

async function expectError(fn, part) {
  let got = "";
  try { await fn(); } catch (err) { got = String(err); }
  assert.ok(got.includes(part), "expected error containing " + part + " but saw " + got);
}
const message = (phase, percentage = 0, reason = "", txn = 41, target = "spiffs") =>
  ({ok:true,transaction_id:txn,target,phase,progress:percentage,reason});

let cases = 0;
{
  const incoming = [message("queued"), message("flashing", 35),
                    message("flashing", 79), message("verified", 100)];
  const messages = [], logs = [];
  const fn = make(async (url) => {
    assert.ok(url.includes("transaction_id=41&target=spiffs"));
    return incoming.shift();
  }, {info:(...a)=>logs.push(a.join(" ")), warn:()=>{}}, messages);
  await fn(41, "spiffs");
  assert.equal(incoming.length, 0);
  assert.ok(messages.some(x=>x.includes("79%")));
  assert.ok(logs.some(x=>x.includes("flash_verified")));
  cases++;
}
{
  const incoming = [new Error("temporary HTTP 503"), new Error("temporary reset"),
                    message("queued"), message("verified",100)];
  const messages=[],warn=[];
  const fn = make(async ()=>{const x=incoming.shift();if(x instanceof Error)throw x;return x},
                  {info:()=>{},warn:(...a)=>warn.push(a)}, messages);
  await fn(41, "spiffs");
  assert.equal(warn.length,1,"transient network errors logged once, not on each poll");
  cases++;
}
{
  const fn=make(async()=>message("error",0,"Flash read-back SHA-256 mismatch"),
                {info:()=>{},warn:()=>{}},[]);
  await expectError(()=>fn(41,"spiffs"),"Flash read-back SHA-256 mismatch");
  cases++;
}
{
  const fn=make(async()=>message("verified",100,"",99),
                {info:()=>{},warn:()=>{}},[]);
  await expectError(()=>fn(41,"spiffs"),"identifiant de transaction ZIP incorrect");
  cases++;
}
{
  const fn=make(async()=>message("idle"),{info:()=>{},warn:()=>{}},[]);
  await expectError(()=>fn(41,"spiffs"),"état de programmation ZIP incohérent");
  cases++;
}
{
  let attempts=0;
  const fn=make(async()=>{attempts++;throw new Error("reset")},
                {info:()=>{},warn:()=>{}},[]);
  await expectError(()=>fn(41,"spiffs"),"après 20 essais");
  assert.equal(attempts,20);
  cases++;
}
console.log("PASS: actual browser Flash polling helper, "+cases+
            " cases (success, transient GET reset, SHA error, stale ID, invalid phase, timeout)");

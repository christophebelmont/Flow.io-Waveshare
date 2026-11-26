#!/usr/bin/env node
/**
 * Host-only ZIP OTA worker handoff regression.
 * Node: node scripts/tests/simulate_local_zip_ota_worker.mjs
 * No ESP32 required. Tests scheduling/admission and source-level ownership.
 */
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const root = new URL("../../", import.meta.url);
const read = path => readFileSync(new URL(path, root), "utf8");
const h = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.h");
const cpp = read("src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp");
const ui = read("src/Modules/TFTModuleS3/TFTModuleS3.cpp");

assert.match(h, /enum class LocalCallKind/);
assert.match(h, /StaticSemaphore_t localCallMutexStorage_/);
assert.match(h, /StaticSemaphore_t localCallDoneStorage_/);
for (const [kind,method] of [
  ["BeginRelease","beginLocalRelease_"],
  ["BeginImage","beginLocalImage_"],
  ["WriteImage","writeLocalImage_"],
  ["EndImage","endLocalImage_"],
  ["Commit","commitLocalRelease_"],
  ["Abort","abortLocalRelease_"]
]) {
  const begin = cpp.indexOf("bool FirmwareUpdateModule::"+method+"(");
  assert.ok(begin>0, "method present "+method);
  const body = cpp.slice(cpp.indexOf("{",begin), cpp.indexOf("{",begin)+900);
  assert.ok(body.includes("LocalCallKind::"+kind), "dispatch "+kind);
  assert.ok(body.indexOf("dispatchLocalCall_") <
            body.indexOf("SemaphoreGuard transactionGuard"),
            "Flash/FS lock after cross-core handoff: "+method);
}
const handle=cpp.slice(cpp.indexOf("bool FirmwareUpdateModule::handleQueuedLocalCall_("),
                       cpp.indexOf("bool FirmwareUpdateModule::beginLocalRelease_("));
for (const token of ["localCallPending_ = false;", "executeLocalCall_(pending)",
                     "localCallInFlight_ = false;", "xSemaphoreGive(localCallDone_)"])
  assert.ok(handle.includes(token), "completion protocol: "+token);
assert.ok(cpp.includes("const bool foreignLocalRpc = localCallInFlight_"));
assert.ok(cpp.includes("if (!localCallOwner_) localCallOwner_ = xTaskGetCurrentTaskHandle()"));
assert.ok(cpp.includes("if (handleQueuedLocalCall_())"));
assert.ok(cpp.includes("if (localCallPending_) {\n            localCallPending_ = false;"));
assert.ok(cpp.includes("xSemaphoreTake(localCallDone_, portMAX_DELAY)"),
          "must not release HTTP body buffer while worker still writes");
assert.ok(ui.includes("setGfxFont_(&FreeSansBold8pt7b, warning, dark)"));

class SimulatedHandoff {
  constructor() { this.busy=false; this.queued=false; this.inFlight=false; this.staged=[]; }
  submit(action, src) {
    if(this.busy || this.queued || this.inFlight) return {accepted:false,reason:"updater busy"};
    this.inFlight=true;
    this.queued=true;
    this.staged.push({action,src});
    return {accepted:true};
  }
  cancelBeforeStart() {
    if(!this.queued) return false;
    this.queued=false; this.inFlight=false;
    this.staged.length=0;
    return true;
  }
  start() {
    assert.equal(this.queued,true);
    this.queued=false;
    return this.staged.shift();
  }
  finish() { assert.equal(this.queued,false); assert.equal(this.inFlight,true); this.inFlight=false; }
}

const bridge=new SimulatedHandoff();
const lifecycle=["BeginRelease","BeginImage","WriteImage","EndImage",
                 "BeginImage","WriteImage","EndImage","Commit"];
for(const step of lifecycle) {
  const bytes=step==="WriteImage"?new Uint8Array([1,2,3,4]):null;
  assert.equal(bridge.submit(step,bytes).accepted,true);
  assert.equal(bridge.submit("foreign",null).accepted,false);
  const work=bridge.start();
  assert.equal(work.action,step);
  if(bytes) assert.equal(work.src[2],3,"source valid until worker completes");
  bridge.finish();
  assert.equal(bridge.inFlight,false);
}
assert.equal(bridge.submit("BeginImage",null).accepted,true);
assert.equal(bridge.cancelBeforeStart(),true,"not-started timeout may cancel");
assert.equal(bridge.inFlight,false);
assert.equal(bridge.submit("Abort",null).accepted,true);
bridge.start();
assert.equal(bridge.cancelBeforeStart(),false,"in-flight writes cannot be cancelled");
bridge.finish();
bridge.busy=true;
assert.equal(bridge.submit("BeginRelease",null).accepted,false);
bridge.busy=false;
console.log("PASS: ZIP worker dispatch, queue rejection, 2 images, safe cancellation, TFT warning");

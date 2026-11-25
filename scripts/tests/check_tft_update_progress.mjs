// node scripts/tests/check_tft_update_progress.mjs
// Source contract: new TFT update view must not change OTA/Nextion workers.
import fs from "node:fs";
import assert from "node:assert/strict";
const root="src/";
const cpp=fs.readFileSync(root+"Modules/TFTModuleS3/TFTModuleS3.cpp","utf8");
const head=fs.readFileSync(root+"Modules/TFTModuleS3/TFTModuleS3.h","utf8");
const fw=fs.readFileSync(root+"Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp","utf8");
const iface=fs.readFileSync(root+"Core/Services/IFirmwareUpdate.h","utf8");
const h=fs.readFileSync(root+"Modules/TFTModuleS3/TftUpdateStages.h","utf8");
for(const marker of [
  "readUpdateSnapshot_", "renderUpdate_", "drawUpdateBrandBitmap_", "updateScreenActive_",
  "kUpdateRenderMinMs", "kUpdateResultHoldMs",
  "kFlowIoLogoBitmap", "kFlowIoWordmarkSplashBitmap",
  "applyBacklight_(true)", "invalidateRenderCache_()",
  "ServiceId::Locale", "tft_update::isEnglish",
  "uint32_t(now - updateTerminalAtMs_)", "updateCompletedStage_",
  "tft_update::renderDirty", "if (dirty.full)", "if (dirty.progress)",
  "if (dirty.details)", "if (dirty.title)"
]) assert.ok((cpp+head).includes(marker),"TFT invariant missing: "+marker);
for(const marker of [
  "FirmwareUpdateDisplaySnapshot",
  "bool (*displaySnapshot)(void* ctx",
  "out->stageProgress = status_.otaStageProgress",
  "out->operationId = status_.operationId",
  "out->message",
  "lastFailure_.operationId == status_.operationId"
]) assert.ok((fw+iface).includes(marker),"snapshot invariant missing: "+marker);
for(const stage of [
  "manifest", "spiffs_download", "spiffs_ram_verify", "spiffs_prepare",
  "spiffs_write", "spiffs_verify", "firmware_download",
  "firmware_ram_verify", "firmware_prepare", "firmware_write",
  "firmware_verify", "activate", "nextion_download", "nextion_verify",
  "nextion_uart", "nextion_upload", "nextion_ack",
  "spiffs_stream", "firmware_stream"
]) assert.ok(h.includes('"'+stage+'"'),"unknown translated stage: "+stage);
assert.match(cpp,/changed && \(urgent \|\| uint32_t\(now - updateLastRenderMs_\) >= kUpdateRenderMinMs\)/);
assert.match(cpp,/if \(updateScreenActive_\) \{/);
assert.doesNotMatch(cpp.slice(cpp.indexOf("void TFTModuleS3::renderUpdate_("),
                               cpp.indexOf("void TFTModuleS3::drawBootLogo_()")),
                    /ESP\.restart\(|Serial1\.write\(|Serial2\.write\(|esp_ota_|HTTPClient|Update\.write/);
const renderer=cpp.slice(cpp.indexOf("void TFTModuleS3::renderUpdate_("),
                         cpp.indexOf("void TFTModuleS3::drawBootLogo_()"));
assert.ok(!renderer.includes("fillRect(0, 111"),"never repaint whole lower update panel");
assert.ok(!renderer.includes("kColorBg"),"update must use black-only background");
assert.ok(renderer.includes("rgb565_(0, 0, 0)"),"black background");
assert.ok(renderer.includes("96, 46, false"),"compact pictogram");
assert.ok(renderer.includes("69, 19, true"),"compact wordmark");
assert.ok(fw.includes('setOtaStage_("nextion_upload"'));
assert.ok(fw.includes('setOtaStage_("activate"'));
console.log("PASS: TFT OTA/Nextion stage labels, bilingual display, bounded redraw, no worker-side drawing");

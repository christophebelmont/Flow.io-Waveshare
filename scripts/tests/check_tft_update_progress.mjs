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
assert.ok(renderer.includes('snprintf(pctText, sizeof(pctText), "%u%%"'),"percentage without STEP/ETAPE");
assert.ok(!renderer.includes('"STEP %u%%"') && !renderer.includes('"ETAPE %u%%"'),"remove STEP/ETAPE");
assert.ok(renderer.includes("if (dirty.progress)") && renderer.includes("if (dirty.details)"),"partial redraw retained");
assert.ok(renderer.includes("display_.fillRect(x, y, filled, 2"),"beveled bar shine");
assert.ok(renderer.includes("display_.fillRect(x, y + 7, filled, 2"),"beveled bar shadow");
assert.ok(renderer.includes("display_.drawFastVLine(x + filled - 1"),"3D progress nose");
assert.ok(renderer.includes("display_.drawLine(12, 154"),"completed step checkmark");
assert.ok(renderer.includes("display_.fillRoundRect(8, 169"),"active step focus strip");
assert.ok(!renderer.includes('display_.print("OK ")'),"no OK in step list");
assert.ok(renderer.indexOf("updateCompletedStage_[0] && !error") < renderer.indexOf("display_.fillRoundRect(8, 169"),"completed above current");
assert.ok(renderer.indexOf("display_.fillRoundRect(8, 164") < renderer.indexOf("drawBounded(detail, 30, 229"),"next below current");

assert.ok(cpp.includes("update.operationId == updateDismissedOperation_"),"persistent terminal result must not reopen update screen");
assert.ok(cpp.includes("updateDismissedOperation_ = update.operationId"),"terminal expiration must mark operation as dismissed");
assert.ok(fw.includes('setOtaStage_("nextion_upload"'));
assert.ok(fw.includes('setOtaStage_("activate"'));
console.log("PASS: TFT OTA/Nextion stage labels, bilingual display, bounded redraw, no worker-side drawing");

assert.ok(renderer.includes('display_.print(english ? "DO NOT" : "NE PAS")'),"warning at top FR/EN");
assert.ok(renderer.includes('display_.print(english ? "POWER OFF" : "ETEINDRE")'),"two-line safety reminder");
assert.ok(renderer.includes("display_.fillRect(6, 3, 98, 29"),"warning confined to free left header");
assert.ok(renderer.includes("drawBounded(detail, 30, 229"),"bottom space reused for next stage");
assert.ok(!renderer.includes('"Ne pas eteindre l\'appareil"'),"bottom footer removed");

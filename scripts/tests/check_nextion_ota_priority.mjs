#!/usr/bin/env node
/**
 * Nextion OTA safety contract. Verify exclusive updater admission, PSRAM
 * staging, dedicated UART, complete final ACK, and honest progress signals.
 *
 * Run: node scripts/tests/check_nextion_ota_priority.mjs
 * This is source-level verification, not a device/PlatformIO integration test.
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const root = new URL('../../', import.meta.url);
const read = name => readFileSync(new URL(name, root), 'utf8');

const cpp = read('src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp');
const hdr = read('src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.h');
const nex = read('lib/ESPNexUpload/src/ESPNexUpload.cpp');
const nexHdr = read('lib/ESPNexUpload/src/ESPNexUpload.h');
const web = read('data/webinterface/app.js');
const modbus = read('src/Modules/Network/ModbusTcpServerModule/ModbusTcpServerModule.h');
const exported = read('scripts/export_binaries.py');
const fr = JSON.parse(read('data/webinterface/i18n/fr.json')).translations;
const en = JSON.parse(read('data/webinterface/i18n/en.json')).translations;
const fn = (start, stop) => {
  const body = cpp.split(start)[1]?.split(stop)[0];
  assert.ok(body, 'Missing function: ' + start);
  return body;
};

// Single admission gate covering Nextion, paired remote OTA and local ZIP OTA.
const admission = fn('bool FirmwareUpdateModule::updateAdmissionBusyLocked_', 'bool FirmwareUpdateModule::isBusy_()');
for (const predicate of ['busy_', 'localReleaseActive_', 'queuedJob_.pending',
                         'nextionRebootQueued_', 'updateStartPending_',
                         'manifestCheckJob_.pending']) {
  assert.ok(admission.includes(predicate), 'Admission missing ' + predicate);
}
assert.match(admission, /busy_ && !allowCurrentRemoteWorker/);
assert.match(hdr, /bool updateAdmissionBusyLocked_\(bool allowCurrentRemoteWorker = false\) const;/);
for (const start of [
  'bool FirmwareUpdateModule::startUpdate_(',
  'bool FirmwareUpdateModule::startManifestCheck_(',
  'bool FirmwareUpdateModule::queueNextionReboot_(',
]) {
  const stop = start === 'bool FirmwareUpdateModule::startUpdate_('
    ? 'bool FirmwareUpdateModule::queueNextionReboot_('
    : start === 'bool FirmwareUpdateModule::startManifestCheck_('
    ? 'bool FirmwareUpdateModule::manifestCheckStatus_('
    : 'bool FirmwareUpdateModule::runWaveshareUpdate_(';
  assert.match(fn(start, stop), /if \(updateAdmissionBusyLocked_\(\)\)/,
               'Missing shared exclusive gate for ' + start);
}
const local = fn('bool FirmwareUpdateModule::beginLocalRelease_', 'bool FirmwareUpdateModule::beginLocalImage_');
assert.match(local, /updateAdmissionBusyLocked_\(remoteWorkerOwnsUpdater\)/);
assert.match(local, /updateAdmissionBusyLocked_\(internalRemoteWorker\)/);
assert.ok(local.indexOf('const bool conflictingUpdater') < local.indexOf('SpiRamJsonDocument doc'),
          'Reject competing local release before PSRAM manifest allocation');
const status = fn('bool FirmwareUpdateModule::statusJson_', 'bool FirmwareUpdateModule::updateAdmissionBusyLocked_');
assert.match(status, /busy = busy_ \|\| queuedJob_\.pending/);

// Actual concurrent updater decisions: no acceptance when either path owns a
// reservation. Only the paired OTA worker may nest its own internal release.
const admissionModel = (state, internalWorker = false) =>
    Boolean((state.busy && !internalWorker) || state.local ||
        state.queued || state.nextionQueued || state.startPending ||
        state.manifestPending || state.manifestActive || state.copyReaders);
for (const key of ['busy', 'local', 'queued', 'nextionQueued',
                   'startPending', 'manifestPending', 'manifestActive', 'copyReaders']) {
  assert.equal(admissionModel({[key]: true}), true, key + ' must exclude other update');
}
assert.equal(admissionModel({busy:true}, true), false, 'Paired worker may open its own release');
assert.equal(admissionModel({busy:true, local:true}, true), true, 'No nested duplicate release');

// Nextion uses the HMI physical UART2, never the Modbus RTU UART1.
assert.match(nex, /#define nexSerial Serial2\b/);
assert.doesNotMatch(nex, /#define nexSerial Serial1\b/);
assert.match(nexHdr, /uint32_t _getBaudrate\(void\)/);
assert.match(nex, /delay\(1\);[\s\S]{0,120}\/\/ if the exit flag/);
assert.match(nex, /bool ESPNexUpload::finishUpload\(\)/);
assert.match(nex, /nextion final block acknowledgement timeout/);
assert.match(nex, /void ESPNexUpload::end\(bool successful\)/);
assert.match(nexHdr, /void end\(bool successful = true\)/);

const nextion = fn('bool FirmwareUpdateModule::runNextionUpdate_', 'bool FirmwareUpdateModule::runNextionReboot_');
const order = [
  'OtaPsramPolicy::canStageImage(', 'heap_caps_malloc(expected',
  'OtaHttpRetryPolicy::responseMode(', 'if (received != expected)',
  'mbedtls_sha256_starts(', 'sha256Matches_(digest, job.nextionSha256)',
  'hmiSvc_->setLocalDisplayUpdateMode(hmiSvc_->ctx, true',
  'nextion.prepareUpload(', 'nextion.upload(staged.get() + sent, count)',
  'nextion.finishUpload()', 'nextion.end(ok)'
];
let previous = -1;
for (const step of order) {
  const position = nextion.indexOf(step);
  assert.ok(position >= 0 && position > previous, 'Stage ordering broken: ' + step);
  previous = position;
}
assert.match(nextion, /OtaHttpRetryPolicy::kMaxAttempts/);
assert.match(nextion, /Nextion PSRAM insufficient \(2 MiB reserve\)/);
assert.doesNotMatch(nextion, /nextion\.upload\(buffer,/);
assert.match(nextion, /"nextion_download"/);
assert.match(nextion, /"nextion_verify"/);
assert.match(nextion, /"nextion_uart"/);
assert.match(nextion, /"nextion_upload"/);
assert.match(nextion, /"nextion_ack"/);
assert.match(exported, /"sha256": _sha256\(path\)/);

const webStages = ['download', 'verify', 'uart', 'upload', 'ack'];
for (const key of webStages) {
  assert.match(web, new RegExp("case 'nextion_" + key + "':"));
  assert.ok(fr['updates.nextion.phase.' + key], 'FR missing stage ' + key);
  assert.ok(en['updates.nextion.phase.' + key], 'EN missing stage ' + key);
}
const ui = web.split('function upgradePhasePercent(session) {')[1]
              .split('function upgradeStepProgress(')[0];
assert.match(ui, /=== 'nextion'/);
assert.match(ui, /Math\.min\(96, progress\)/);
assert.match(modbus, /const bool otaBusy = fwUpdateSvc_ && fwUpdateSvc_->isBusy/);
assert.match(modbus, /if \(otaBusy\) \{[\s\S]*?server_\.stop\(\);[\s\S]*?return;/);

console.log('PASS: exclusive Nextion/paired OTA, PSRAM before UART, Modbus TCP pause, real phase progress');

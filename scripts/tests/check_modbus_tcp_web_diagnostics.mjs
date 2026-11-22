#!/usr/bin/env node
/**
 * Source-level guard for Modbus TCP PSRAM diagnostics in Web/boot logs.
 * This does not compile firmware or simulate active Modbus clients.
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const root = new URL('../../', import.meta.url);
const read = p => readFileSync(new URL(p, root), 'utf8');
const header = read('src/Modules/Network/ModbusTcpServerModule/ModbusTcpServerModule.h');
const cpp = read('src/Modules/Network/ModbusTcpServerModule/ModbusTcpServerModule.cpp');
const ids = read('src/Core/LogModuleIds.h');
const hub = read('src/Core/LogHub.cpp');
const server = read('src/Modules/Network/ModbusTcpServerModule/ModbusTcpSocketServer.h');
const logger = read('src/Core/Services/ILogger.h');

const max = Number(logger.match(/constexpr int LOG_MSG_MAX = (\d+);/)?.[1]);
assert.equal(max, 160, 'Re-evaluate log length bounds when LogEntry changes');
assert.match(ids, /ModbusTcpServerModule = 35/);
assert.match(ids, /case ModuleId::ModbusTcpServer: return .*ModbusTcpServerModule;/);
assert.match(ids, /case LogModuleIdValue::ModbusTcpServerModule: return "modbustcp";/);
assert.match(hub, /case LogModuleIdValue::ModbusTcpServerModule:/);
assert.match(header, /#include "Core\/Log.h"/);
assert.match(header, /logWebSnapshot_\(now, stationConnected, true\)/);
assert.match(header, /logWebSnapshot_\(now, stationConnected, false\)/);
const loop = header.split('void loop() override {')[1]?.split('// MQTT JSON extension')[0];
assert.ok(loop, 'module loop not found');
assert.ok(loop.indexOf('logWebSnapshot_(now, stationConnected, true)') <
          loop.indexOf('if (!eligible_) return;'),
          'OTA Web telemetry must run when listeners are disabled');
assert.ok(loop.indexOf('logWebSnapshot_(now, stationConnected, false)') <
          loop.indexOf('if (!eligible_) return;'),
          'Periodic Web telemetry must run when listeners are disabled');
assert.match(header, /lastWebDiagnosticsMs_ == 0U/);
assert.match(header, /uint32_t\(now - lastWebDiagnosticsMs_\) >= 60000U/);
assert.match(header, /uint32_t\(now - lastOtaTraceMs_\) >= 5000U/);
assert.match(header, /uint32_t\(now - lastPsramFailureWebMs_\) >= 60000U/);
assert.match(header, /Log::warn\(kLogModule_,\s*"\[MODBUS-PSRAM\] alloc_failed/);
assert.match(header, /Log::info\(kLogModule_,\s*"\[MODBUS-PSRAM\] pool_ready/);
assert.match(header, /if \(!server_\.begin\(/);
assert.match(server, /heap_caps_calloc\(1U, sizeof\(FrameStorage\),\s*MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\)/);

const functionBody = cpp.split('void ModbusTcpServerModule::logWebSnapshot_(')[1]
  ?.split('void ModbusTcpServerModule::init(')[0];
assert.ok(functionBody && functionBody.includes('if (!Log::hub()) return;'),
  'no Web logger or no guard when LogHub is absent');
const formats = [
  ...[...functionBody.matchAll(/Log::(?:info|warn)\(kLogModule_,\s*"([^"]+)"/g)].map(m => m[1]),
  ...[...header.matchAll(/Log::(?:info|warn)\(kLogModule_,\s*"([^"]+)"/g)].map(m => m[1])
];
assert.ok(formats.length >= 12, 'missing central Modbus Web diagnostic formats');
for (const prefix of [
  '[MODBUS-PSRAM]', '[MODBUS-MEM]', '[MODBUS-TCP]',
  '[MODBUS-ERR]', '[MODBUS-LAT]', '[OTA-NET]', '[OTA-TCP]'
]) {
  assert.ok(formats.some(f => f.startsWith(prefix)), 'missing ' + prefix);
}
function worstLength(format) {
  return format.replace(/%(?:lu|u|d|p|s)/g, spec =>
    spec === '%lu' ? '4294967295' :
    spec === '%u' ? '65535' :
    spec === '%d' ? '-1' :
    spec === '%p' ? '0xffffffff' : 'z'.repeat(32)).length;
}
for (const format of formats) {
  const length = worstLength(format);
  assert.ok(length < max,
    'Truncated Web diagnostic (' + length + '/' + max + '): ' + format);
}
console.log('PASS: Modbus PSRAM/TCP/OTA Web diagnostics, independent of port state, bounded log lines');

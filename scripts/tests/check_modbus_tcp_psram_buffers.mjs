#!/usr/bin/env node
/* Static contract, not an ESP32 compile/memory measurement. */
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const root = new URL('../../', import.meta.url);
const load = p => readFileSync(new URL(p, root), 'utf8');
const s = load('src/Modules/Network/ModbusTcpServerModule/ModbusTcpSessions.h');
const b = load('src/Modules/Network/ModbusTcpServerModule/ModbusTcpBridge.h');
const t = load('src/Modules/Network/ModbusTcpServerModule/ModbusTcpSocketServer.h');
const m = load('src/Modules/Network/ModbusTcpServerModule/ModbusTcpServerModule.h');
assert.match(t, /uint8_t rx\[kMaxClients\]\[ModbusTcpAdu::kMaxAdu\]/);
assert.match(t, /uint8_t tx\[kMaxClients\]\[ModbusTcpAdu::kMaxAdu\]/);
assert.match(t, /heap_caps_calloc\(1U, sizeof\(FrameStorage\),\s*MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\)/);
assert.match(t, /if \(!memory\)\s*\{[\s\S]*?return false;/);
assert.match(t, /sessions_\.attachFrames\(frameStorage_->rx\)/);
assert.match(t, /bridges_\[i\]\.attachOutput\(frameStorage_->tx\[i\]\)/);
assert.match(t, /FrameStorage\* frameStorage_ = nullptr/);
assert.doesNotMatch(s, /uint8_t frame\[ModbusTcpAdu::kMaxAdu\]/);
assert.match(s, /uint8_t \(\*frames_\)\[ModbusTcpAdu::kMaxAdu\] = nullptr/);
assert.doesNotMatch(b, /uint8_t output_\[ModbusTcpAdu::kMaxAdu\]/);
assert.match(b, /uint8_t\* output_ = nullptr/);
assert.doesNotMatch(b, /sizeof\(output_\)/);
assert.match(m, /lastPsramStartFailureMs_/);
assert.match(m, /if \(!server_\.begin\(/);
assert.match(load('test/host/modbus_tcp_sessions.cpp'), /sessions\.attachFrames\(externalRx\)/);
assert.match(load('test/host/modbus_tcp_bridge.cpp'), /slot\.attachOutput\(externalTx\)/);
console.log('PASS: TCP frame buffers use PSRAM and no internal heap fallback');

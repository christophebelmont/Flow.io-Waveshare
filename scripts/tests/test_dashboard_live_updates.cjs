'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const app = fs.readFileSync(path.join(__dirname, '../../data/webinterface/app.js'), 'utf8');
const controllerCode = app.slice(app.indexOf('    function createDashboardLiveUpdates('),
  app.indexOf('    async function refreshDashboardLiveDomains('));
const cacheCode = app.slice(app.indexOf('    function invalidatePoolDashboardSlots('),
  app.indexOf('    async function fetchPoolSondeSlots('));

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
const settle = async () => { for (let i = 0; i < 12; ++i) await Promise.resolve(); };
function fixture() {
  let now = 100000, timerId = 0;
  const timers = new Map(), sources = [], reads = [];
  const state = { active: true, supported: true, invalidations: 0, read: async () => {} };
  const context = vm.createContext({ Date: { now: () => now },
    setTimeout: (fn, delay) => { timers.set(++timerId, { fn, at: now + delay }); return timerId; },
    clearTimeout: id => timers.delete(id) });
  vm.runInContext(controllerCode, context);
  const controller = context.createDashboardLiveUpdates({
    isActive: () => state.active, canStream: () => state.supported,
    invalidate: () => ++state.invalidations,
    refresh: async mask => { reads.push(mask); await state.read(mask); },
    openSource: () => {
      const source = { readyState: 0, closed: false, listener: null,
        close() { this.closed = true; this.readyState = 2; },
        addEventListener(name, listener) { assert.equal(name, 'runtime'); this.listener = listener; },
        open() { this.readyState = 1; this.onopen(); },
        event(revision, domains) { this.listener({ data: JSON.stringify({ revision, domains }) }); },
        fail(closed = false) { this.readyState = closed ? 2 : 0; this.onerror(); } };
      sources.push(source);
      return source;
    }
  });
  async function tick(ms) {
    const end = now + ms;
    for (;;) {
      const next = [...timers].filter(([, timer]) => timer.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
      if (!next) break;
      now = next[1].at; timers.delete(next[0]); next[1].fn(); await settle();
    }
    now = end; await settle();
  }
  return { controller, state, reads, sources, tick };
}

async function testLiveUpdates() {
  const f = fixture(), c = f.controller;
  c.start(); c.start(); assert.equal(f.sources.length, 1);
  const s = f.sources[0]; s.open(); s.event(1, 15);
  await f.tick(80); assert.deepEqual(f.reads, [15], 'Connection resynchronizes all cards once');
  s.event(2, 2); s.event(3, 4);
  await f.tick(80); assert.equal(f.reads.at(-1), 6, 'Equipment and alarm changes coalesce');
  const before = f.reads.length;
  s.event(3, 0); await f.tick(80); assert.equal(f.reads.length, before, 'Heartbeat does not reread cards');
  c.poll(); await f.tick(80); assert.equal(f.reads.at(-1), 8, 'Connected polling reads sensors only');
  s.event(5, 2); await f.tick(80); assert.equal(f.reads.at(-1), 15, 'Missing revision resynchronizes all cards');
  s.event(6, 0); await f.tick(80); assert.equal(f.reads.at(-1), 15, 'Heartbeat catches missed final notification');
  const read = deferred(); f.state.read = () => read.promise;
  s.event(7, 2); await f.tick(80);
  s.event(8, 4); assert(f.state.invalidations > 0);
  read.resolve(); f.state.read = async () => {}; await settle(); await f.tick(80);
  assert.deepEqual(f.reads.slice(-2), [2, 4], 'Notification during a read triggers another read');
  s.fail(); c.poll(); await f.tick(80); assert.equal(f.reads.at(-1), 15, 'Transport loss falls back to all cards');
  s.open(); s.event(8, 15); await f.tick(80); assert.equal(f.reads.at(-1), 15);
  s.fail(true); await f.tick(3000); assert.equal(f.sources.length, 2, 'HTTP rejection is retried');
  const latest = f.sources.at(-1); latest.open(); latest.event(9, 15); await f.tick(80);
  c.stop(); assert(latest.closed); const stopped = f.reads.length;
  s.event(10, 2); latest.event(10, 4); c.poll(); c.stop();
  await f.tick(5000); assert.equal(f.reads.length, stopped, 'Closed sources cannot refresh the page');
}

async function testRecoveryAndFallback() {
  const f = fixture(); f.state.supported = false;
  f.controller.start(); assert.equal(f.sources.length, 0);
  f.controller.poll(); await f.tick(80); assert.deepEqual(f.reads, [15], 'Old firmware retains polling');
  f.state.active = false; f.controller.poll(); await f.tick(10000); assert.equal(f.reads.length, 1);
  f.state.active = true; f.state.supported = true; f.controller.start();
  const s = f.sources.at(-1); s.open(); s.event(1, 15); await f.tick(80);
  f.state.read = async () => { throw new Error('temporary read failure'); };
  s.event(2, 4); await f.tick(80); const failed = f.reads.length;
  await f.tick(1999); assert.equal(f.reads.length, failed, 'Failed reads back off');
  f.state.read = async () => {}; await f.tick(1); assert.equal(f.reads.at(-1), 4, 'Failed domain is retried');
  for (let i = 0; i < 4; ++i) { await f.tick(15000); s.event(2, 0); }
  f.controller.poll(); await f.tick(80); assert.equal(f.reads.at(-1), 15, 'Periodic reconciliation covers lost events');
  await f.tick(35000); f.controller.poll(); await f.tick(80);
  assert(s.closed); assert.equal(f.reads.at(-1), 15, 'Stalled stream reconnects and resynchronizes');
  f.controller.stop();
}

async function testCacheInvalidationDuringRead() {
  const requests = [];
  const context = vm.createContext({ Date,
    fetchOkJson: () => { const request = deferred(); requests.push(request); return request.promise; } });
  vm.runInContext('let poolDashboardSlotsGeneration = 0, poolDashboardSlotsCache = null, ' +
    'poolDashboardSlotsFetchedAt = 0, poolDashboardSlotsLoadPromise = null;\n' + cacheCode, context);
  const first = context.fetchPoolDashboardSlots(), second = context.fetchPoolDashboardSlots();
  assert.equal(requests.length, 1, 'Concurrent domain reads share the snapshot request');
  context.invalidatePoolDashboardSlots(); requests[0].resolve({ condition: false }); await settle();
  assert.equal(requests.length, 2, 'Invalidation discards the in-flight old snapshot');
  requests[1].resolve({ condition: true });
  assert.equal((await first).condition, true); assert.equal((await second).condition, true);
  assert.equal((await context.fetchPoolDashboardSlots()).condition, true);
  assert.equal(requests.length, 2, 'Only the fresh snapshot is cached');
}

(async () => {
  await testLiveUpdates(); await testRecoveryAndFallback(); await testCacheInvalidationDuringRead();
  console.log('Dashboard live updates: batching, reconnect, fallback, recovery and cache tests passed');
})().catch(error => { console.error(error); process.exitCode = 1; });

'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { loadFunctions } = require('./app_function_test_support.cjs');

async function main() {
  const source = fs.readFileSync('data/webinterface/app.js', 'utf8');
  const responses = [];
  let requests = 0;
  let visible = true;
  const dialog = {
    open: false,
    showModal() { this.open = true; },
    close() { this.open = false; }
  };
  const context = vm.createContext({
    infoHealthDialog: dialog,
    infoHealthStatus: {},
    infoTaskHealth: {},
    infoTaskHealthRows: {},
    infoEventBusHealth: {},
    infoEventBusHealthRows: {},
    infoHealthData: null,
    infoHealthFetchFailed: false,
    infoHealthRefreshPromise: null,
    tr: (key, fallback) => fallback,
    currentWebLocaleTag: () => 'fr-FR',
    isInfoPageVisible: () => visible,
    fetchOkJson(url) {
      assert.equal(url, '/api/system/health');
      ++requests;
      return responses.shift()();
    }
  });
  loadFunctions(source, context, [
    'buildInfoMetricRow', 'renderInfoMetricRows', 'formatInfoCount',
    'formatRuntimeDurationMs', 'formatInfoHealthDuration', 'renderInfoHealth', 'refreshInfoHealth',
    'openInfoHealthDialog'
  ]);

  await context.refreshInfoHealth();
  assert.equal(requests, 0);
  responses.push(() => Promise.resolve({ ok: true }));
  await context.openInfoHealthDialog();
  assert(dialog.open);
  assert.match(context.infoHealthStatus.textContent, /premier relevé/);
  assert(context.infoTaskHealth.hidden && context.infoEventBusHealth.hidden);

  responses.push(() => Promise.resolve({
    ok: true,
    sample_age_ms: 550,
    tasks: {
      running: 23, below_stack: 0, late_starts: 4, work_overruns: 1,
      lowest_stack: { task: 'io<test>', free_b: 700, threshold_b: 512, sample_age_ms: 250 },
      longest_work: { task: 'poollogic', duration_us: 2500 }
    },
    event_bus: { capacity: 40, queued: 0, peak: 40, max_delay_us: 35, drops: 1, control_drops: 1, retries: 2 }
  }));
  await Promise.all([context.refreshInfoHealth(), context.refreshInfoHealth()]);
  assert.equal(requests, 2);
  assert(!context.infoTaskHealth.hidden && !context.infoEventBusHealth.hidden);
  assert.match(context.infoHealthStatus.textContent, /550 ms/);
  assert.match(context.infoTaskHealthRows.innerHTML, /io&lt;test&gt; · 700 \/ 512 B/);
  assert.match(context.infoTaskHealthRows.innerHTML, /Mesuré il y a 250 ms/);
  assert.match(context.infoTaskHealthRows.innerHTML, /poollogic · 2,5 ms/);
  assert.match(context.infoEventBusHealthRows.innerHTML, /0 \/ 40.*40 \/ 40/);
  assert.match(context.infoEventBusHealthRows.innerHTML, /35 µs/);

  const previousRows = context.infoEventBusHealthRows.innerHTML;
  responses.push(() => Promise.reject(new Error('offline')));
  await context.refreshInfoHealth();
  assert.match(context.infoHealthStatus.textContent, /Dernier relevé affiché/);
  assert.equal(context.infoEventBusHealthRows.innerHTML, previousRows);
  visible = false;
  await context.refreshInfoHealth();
  assert.equal(requests, 3);
  visible = true;
  dialog.close();
  await context.refreshInfoHealth();
  assert.equal(requests, 3);
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});

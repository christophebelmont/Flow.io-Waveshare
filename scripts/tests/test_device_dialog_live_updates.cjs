'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { chromium } = require('playwright');
const { project, loadDialog, installDialog } = require('./runtime_action_dialog_test_support.cjs');
const entry = loadDialog('PoolDeviceModule', 5, 2305);
const config = entry.displayConfig.actionDialog;
const app = fs.readFileSync(path.join(project, 'data/webinterface/app.js'), 'utf8');
const liveCode = app.slice(app.indexOf('    function createDashboardLiveUpdates('),
  app.indexOf('    function stopPoolMeasuresTimer('));
const clients = new Set(), commands = [], reads = [];
let revision = 1, holdRead = false, failRead = false, releaseRead = null;
const devices = [
  { value: 0, label: 'Filtration (pd0)', name: 'Filtration', deviceId: 'pd0', actualOn: false, controllable: true },
  { value: 7, label: 'COMP (pd7)', name: 'COMP', deviceId: 'pd7', actualOn: false, controllable: true }
].map(device => ({ ...device, running: { day_s: 0, week_s: 0, month_s: 0, total_s: 0 },
  injected: { day_ml: 0, week_ml: 0, month_ml: 0, total_ml: 0 } }));
function event(mask) {
  ++revision;
  for (const client of clients) client.write(`event: runtime\nid: ${revision}\ndata: ${JSON.stringify({ revision, domains: mask })}\n\n`);
}
const server = http.createServer((request, response) => {
  if (request.url === '/api/runtime/events') {
    response.writeHead(200, { 'Content-Type': 'text/event-stream' }); response.flushHeaders();
    clients.add(response);
    response.write(`event: runtime\nretry: 100\ndata: ${JSON.stringify({ revision, domains: 15 })}\n\n`);
    request.on('close', () => clients.delete(response));
  } else if (request.url === config.optionsUrl) {
    reads.push(revision);
    const snapshot = JSON.stringify({ options: devices });
    const reply = () => { response.writeHead(failRead ? 503 : 200, { 'Content-Type': 'application/json' });
      response.end(failRead ? '{"error":"Liste indisponible"}' : snapshot); };
    if (holdRead) { holdRead = false; releaseRead = reply; } else reply();
  } else if (request.url === '/api/runtime/action') {
    let body = '';
    request.on('data', chunk => { body += chunk; });
    request.on('end', () => {
      const command = Object.fromEntries(new URLSearchParams(body)); commands.push(command);
      // Acceptance and the later physical transition are separate firmware events.
      setTimeout(() => {
        response.writeHead(200, { 'Content-Type': 'application/json' }); response.end('{"ok":true}');
        if (command.action_id === 'set_device') setTimeout(() => {
          devices.find(device => device.value === Number(command.target)).actualOn = command.input === 'true';
          event(2);
        }, 300);
      }, 200);
    });
  } else {
    response.writeHead(200, { 'Content-Type': 'text/html' });
    response.end('<html lang="fr"><body><div class="status-card"><h3>Équipements</h3></div></body></html>');
  }
});
async function waitFor(predicate) {
  const deadline = Date.now() + 3000;
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error('Test fixture timed out');
    await new Promise(resolve => setTimeout(resolve, 10));
  }
}
async function main() {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const browser = await chromium.launch({ headless: true,
    ...(process.env.FLOWIO_TEST_BROWSER ? { executablePath: process.env.FLOWIO_TEST_BROWSER } : {}) });
  try {
    const page = await browser.newPage();
    const errors = []; page.on('pageerror', error => errors.push(error.message));
    await page.goto(`http://127.0.0.1:${server.address().port}`);
    await page.addStyleTag({ content: fs.readFileSync(path.join(project, 'data/webinterface/app-core.css'), 'utf8') });
    await page.evaluate(() => {
      window.testState = { invalidations: 0, refreshes: [] };
      window.fetchOkJson = async url => {
        const response = await fetch(url); const data = await response.json();
        if (!response.ok) throw new Error(data.error); return data;
      };
      window.fetchWithBusyRetry = (url, request) => fetch(url, request);
    });
    await installDialog(page, entry);
    await page.evaluate(code => {
      window.poolMeasureDomainState = Object.fromEntries(['mode', 'equipements', 'alarm', 'sondes'].map(domain => [domain, { error: '' }]));
      const script = document.createElement('script');
      script.textContent = code + '\nwindow.live = createDashboardLiveUpdates({' +
        'isActive:()=>true,canStream:()=>true,openSource:()=>new EventSource("/api/runtime/events"),' +
        'invalidate:()=>invalidatePoolDashboardSlots(),refresh:domains=>refreshDashboardLiveDomains(domains)}); live.start();';
      document.body.appendChild(script);
    }, liveCode);
    await page.waitForFunction(() => live.isConnected() && testState.refreshes.includes('overview'));
    await page.getByRole('button', { name: config.buttonText, exact: true }).click();
    const row = label => page.locator('tbody tr').filter({ has: page.getByRole('rowheader', { name: label, exact: true }) });
    const pump = row('Filtration (pd0)').getByRole('switch');
    const comp = row('COMP (pd7)').getByRole('switch');
    const resetComp = page.getByRole('button', { name: config.rowButtonText + ' — COMP (pd7)', exact: true });
    await comp.waitFor();
    await page.evaluate(() => {
      window.stableTable = document.querySelector('dialog table');
      window.stableCells = [...stableTable.querySelectorAll('td, th, input')];
      window.unchangedMutations = [];
      window.unchangedObserver = new MutationObserver(records => unchangedMutations.push(...records));
      unchangedObserver.observe(stableTable.querySelector('tbody tr'), { attributes: true, childList: true, characterData: true, subtree: true });
    });
    const beforeChange = Date.now(); devices[1].actualOn = true; devices[1].running.day_s = 1; event(2);
    await page.waitForFunction(() => document.querySelector('input[aria-label="On/Off — COMP (pd7)"]').checked);
    assert(Date.now() - beforeChange < 2000, 'An off-card device updates through SSE');
    assert(await comp.isChecked()); assert.equal(commands.length, 0, 'Live reads do not send commands');
    assert(await page.evaluate(() => stableTable === document.querySelector('dialog table') &&
      stableCells.every((cell, index) => cell === stableTable.querySelectorAll('td, th, input')[index])),
      'Live updates retain the table, cells and switches');
    assert.equal(await page.evaluate(() => unchangedMutations.length), 0, 'Unchanged device values and controls are not rewritten');
    assert.equal(await row('COMP (pd7)').locator('.runtime-counter-duration').first().textContent(), '00:00:01');
    await page.evaluate(() => unchangedObserver.disconnect());

    await resetComp.click();
    const confirm = page.getByRole('button', { name: config.confirmButtonText, exact: true });
    await confirm.evaluate(element => { window.stableConfirmation = element; });
    devices[0].actualOn = true; event(2);
    await page.waitForFunction(() => document.querySelector('input[aria-label="On/Off — Filtration (pd0)"]').checked);
    assert.equal(await page.locator('.runtime-counter-confirmation').count(), 1, 'Live updates preserve reset confirmation');
    assert(await confirm.evaluate(element => element === document.activeElement), 'Confirmation focus survives live reads');
    assert(await confirm.evaluate(element => element === stableConfirmation), 'Confirmation controls stay mounted');
    await page.keyboard.press('Escape');
    assert.equal(await page.locator('.runtime-counter-confirmation').count(), 0);

    holdRead = true; devices[1].actualOn = false; event(2); await waitFor(() => releaseRead);
    assert(await comp.isEnabled(), 'A background read does not dim all switches');
    assert(await resetComp.isEnabled(), 'Background reads leave actions available');
    devices[1].actualOn = true; devices[1].running.day_s = 2; event(2);
    const firstReply = releaseRead; releaseRead = null; firstReply();
    await waitFor(() => reads.at(-1) === revision);
    await page.waitForFunction(() => document.querySelector('input[aria-label="On/Off — COMP (pd7)"]').checked &&
      [...document.querySelectorAll('tbody tr')].find(row => row.querySelector('th')?.getAttribute('aria-label') === 'COMP (pd7)')
        .querySelector('.runtime-counter-duration').textContent === '00:00:02');
    assert(await comp.isChecked(), 'A change during a read gets a fresh follow-up snapshot');

    holdRead = true; event(2); await waitFor(() => releaseRead);
    await pump.click();
    assert(await pump.isDisabled(), 'A command still locks switches during a background read');
    assert.equal(commands.length, 0, 'Commands wait for the earlier background snapshot');
    const beforeCommandReply = releaseRead; releaseRead = null; beforeCommandReply();
    devices[1].actualOn = false; event(2); // Notification while the popup command owns its refresh.
    await page.waitForFunction(() => !document.querySelector('input[aria-label="On/Off — Filtration (pd0)"]').checked &&
      !document.querySelector('input[aria-label="On/Off — COMP (pd7)"]').checked);
    assert.deepEqual(commands, [{ runtime_id: '2305', action_id: 'set_device', input: 'false', target: '0' }],
      'The switch keeps using the declared equipment command');

    failRead = true; event(2);
    await page.waitForFunction(() => document.querySelector('dialog [role="status"]').textContent.includes('Liste indisponible'));
    failRead = false;
    await page.waitForFunction(() => document.querySelector('input[aria-label="On/Off — COMP (pd7)"]') &&
      document.querySelector('dialog [role="status"]').textContent === '');
    assert(!(await comp.isChecked()), 'Failed live reads recover without retaining stale switches');
    await page.getByRole('button', { name: 'Fermer', exact: true }).click();
    const closedReads = reads.length; event(2);
    await page.waitForFunction(() => !document.querySelector('dialog'));
    await new Promise(resolve => setTimeout(resolve, 200));
    assert.equal(reads.length, closedReads, 'A closed popup no longer loads options');
    await page.evaluate(() => live.stop());
    assert.deepEqual(errors, []);
    console.log('Equipment popup SSE: stable cells, unchanged DOM, live states, confirmations, concurrent commands and recovery passed');
  } finally {
    await browser.close();
    if (releaseRead) releaseRead();
    for (const client of clients) client.end();
    await new Promise(resolve => server.close(resolve));
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });

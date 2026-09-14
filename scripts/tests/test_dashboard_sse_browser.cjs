'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { chromium } = require('playwright');
const app = fs.readFileSync(path.join(__dirname, '../../data/webinterface/app.js'), 'utf8');
const liveCode = app.slice(app.indexOf('    function createDashboardLiveUpdates('),
  app.indexOf('    function stopPoolMeasuresTimer('));
const clients = new Set();
let revision = 1, connections = 0;
const state = { mode: false, equipements: false, alarm: false, sondes: 23 };
function send(client, domains) {
  client.write(`event: runtime\nid: ${revision}\nretry: 100\ndata: ${JSON.stringify({ revision, domains })}\n\n`);
}
function change(domain, value, mask) {
  state[domain] = value; ++revision;
  for (const client of clients) send(client, mask);
}
const server = http.createServer((request, response) => {
  const url = new URL(request.url, 'http://localhost');
  if (url.pathname === '/api/runtime/events') {
    response.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
    response.flushHeaders(); clients.add(response); ++connections; send(response, 15);
    request.on('close', () => clients.delete(response));
  } else if (url.pathname === '/snapshot') {
    const domain = url.searchParams.get('domain');
    response.writeHead(200, { 'Content-Type': 'application/json' });
    response.end(JSON.stringify({ value: state[domain] }));
  } else {
    response.writeHead(200, { 'Content-Type': 'text/html' });
    response.end('<html><body><input id="mode" type="checkbox"><input id="equipements" type="checkbox">' +
      '<div id="alarm"></div><div id="sondes"></div></body></html>');
  }
});
async function main() {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const browser = await chromium.launch({ headless: true,
    ...(process.env.FLOWIO_TEST_BROWSER ? { executablePath: process.env.FLOWIO_TEST_BROWSER } : {}) });
  try {
    const origin = `http://127.0.0.1:${server.address().port}`;
    const pages = [await browser.newPage(), await browser.newPage()];
    for (const page of pages) {
      await page.goto(origin);
      await page.evaluate(code => {
        window.reads = []; window.invalidations = 0; window.active = true;
        window.poolMeasureDomainState = Object.fromEntries(['mode', 'equipements', 'alarm', 'sondes'].map(key => [key, { error: '' }]));
        window.loadPoolMeasureDomain = async domain => {
          reads.push(domain);
          const snapshot = await (await fetch('/snapshot?domain=' + domain)).json();
          const element = document.getElementById(domain);
          if (element instanceof HTMLInputElement) element.checked = snapshot.value;
          else element.textContent = String(snapshot.value);
        };
        window.refreshPoolOverview = async () => {};
        window.refreshRuntimeActionDialog = async () => {};
        // Keep the production dispatcher in the same lexical scope as its controller.
        window.live = new Function(code + '\nreturn createDashboardLiveUpdates({' +
          'isActive:()=>active,canStream:()=>true,openSource:()=>new EventSource("/api/runtime/events"),' +
          'invalidate:()=>++invalidations,refresh:domains=>refreshDashboardLiveDomains(domains)});')();
        live.start();
      }, liveCode);
      await page.waitForFunction(() => document.getElementById('sondes').textContent === '23');
      await page.evaluate(() => { reads.length = 0; });
    }
    assert.equal(clients.size, 2, 'Two visible dashboards receive the same firmware notifications');
    const started = Date.now(); change('equipements', true, 2);
    for (const page of pages) await page.waitForFunction(() => document.getElementById('equipements').checked);
    assert(Date.now() - started < 2000, 'External device change appears before the ten-second poll');
    for (const page of pages) {
      assert.deepEqual(await page.evaluate(() => reads), ['equipements'], 'Only the affected measure domain is read');
      await page.evaluate(() => { reads.length = 0; });
    }
    change('alarm', true, 4);
    for (const page of pages) await page.waitForFunction(() => document.getElementById('alarm').textContent === 'true');
    for (const page of pages) assert.deepEqual(await page.evaluate(() => reads), ['alarm']);
    const previousConnections = connections;
    for (const client of [...clients]) client.end();
    state.mode = true; ++revision;
    for (const page of pages) await page.waitForFunction(() => document.getElementById('mode').checked);
    for (const page of pages) await page.waitForFunction(() => live.isConnected());
    assert(connections >= previousConnections + 2, 'Native EventSource reconnects both dashboards');
    await pages[0].evaluate(() => { active = false; live.stop(); });
    const stoppedReads = await pages[0].evaluate(() => reads.length);
    change('alarm', false, 4);
    await pages[1].waitForFunction(() => document.getElementById('alarm').textContent === 'false');
    assert.equal(await pages[0].evaluate(() => reads.length), stoppedReads, 'Leaving the dashboard stops its stream');
    console.log('Dashboard SSE browser: two clients, live switches/alarms, selective reads and reconnect passed');
  } finally {
    await browser.close();
    for (const client of clients) client.end();
    await new Promise(resolve => server.close(resolve));
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });

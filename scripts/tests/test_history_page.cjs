'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const root = path.resolve(__dirname, '../..');
const app = fs.readFileSync(path.join(root, 'data/webinterface/app.js'), 'utf8');
const code = app.slice(app.indexOf('    // History is fetched only'), app.indexOf('    function showPage('));
const shell = fs.readFileSync(path.join(root, 'data/webinterface/sh.html'), 'utf8');
const section = shell.slice(shell.indexOf('      <section id="page-history"'), shell.indexOf('      <section id="page-activity-log"'));
(async () => {
  const browser = await chromium.launch({ headless: true, executablePath: process.env.FLOWIO_TEST_BROWSER || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' });
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 1050 } });
    await page.setContent('<html lang="fr"><body><main style="padding:24px;max-width:1150px;margin:auto">' + section.replace('class="page"', 'class="page active"') + '</main></body></html>');
    await page.addStyleTag({ content: fs.readFileSync(path.join(root, 'data/webinterface/app-core.css'), 'utf8') });
    await page.evaluate(code => {
      window.tr = (key, fallback) => fallback || key;
      window.currentWebLocaleTag = () => 'fr-FR';
      const day = (average, start) => average == null ? null : ({ start, average, min: average - .2, max: average + .3, coverage_ms: 3600000, delta: average, discontinuities: 1 });
      window.fixture = { ok: true, ready: true, values: [
        { id: 0, name: 'Entrée A00', group: 'Entrées analogiques', mode: 0, type: 4, unit: 0, available: true, pending_restart: false,
          days: [day(25.4, 1790294400), day(25.2, 1790208000), day(null, 0), day(24.8, 1790035200), day(24.6, 1789948800), day(24.4, 1789862400), day(24.2, 1789776000), day(24.0, 1789689600)] },
        { id: 32, name: 'I00 · Impulsions', group: 'Compteurs', mode: 2, type: 3, unit: 1, available: false, pending_restart: true,
          days: [day(120, 1790294400), day(80, 1790208000), day(null, 0), day(60, 1790035200), day(50, 1789948800), day(40, 1789862400), day(30, 1789776000), day(20, 1789689600)] }
      ] };
      window.requests = [];
      window.responseMode = 0;
      window.fetchOkJson = async (url) => {
        requests.push(url);
        if (responseMode === 1) throw new Error('offline');
        if (url.includes('/value?')) {
          const gauge = /[?&]id=0(&|$)/.test(url);
          const hour = i => ({ start_utc: 1790294400 - i * 3600, current: i === 0, average: 25 - i * 0.1, min: 24 - i * 0.1, max: 26 - i * 0.1, delta: 10 + i, coverage_ms: 3600000, discontinuities: i === 3 ? 1 : 0, boundary_uncertain: i === 5, raw_delta: '18446744073709551615' });
          return { ok: true, mode: gauge ? 0 : 2, type: gauge ? 4 : 3, unit: gauge ? 0 : 1, records: responseMode === 2 ? [] : Array.from({ length: 24 }, (_, i) => hour(i)) };
        }
        return structuredClone(fixture);
      };
      const script = document.createElement('script'); script.textContent = code; document.body.appendChild(script);
    }, code);
    await page.evaluate(() => loadHistory());
    // One row per valid+historized value; eight day columns showing the daily mean.
    assert.equal(await page.locator('#historyDays tbody tr').count(), 2);
    assert.equal(await page.locator('#historyDays thead th').count(), 9);
    assert.ok((await page.locator('#historyDays tbody tr').first().locator('td').nth(8).textContent()).includes('25,4'));
    assert.equal(await page.locator('#historyDays tbody tr').first().locator('td').nth(6).textContent(), '—');
    // Clicking a value opens the min/max popup; a day without a stored record stays inert.
    assert.equal(await page.locator('#historyDays tbody tr').first().locator('.history-cell-btn').count(), 7);
    await page.locator('#historyDays tbody tr').first().locator('.history-cell-btn').first().click();
    assert.notEqual(await page.locator('#historyDetailDialog').getAttribute('open'), null);
    assert.ok((await page.locator('#historyDetailBody').textContent()).includes('Min'));
    assert.ok((await page.locator('#historyDetailBody').textContent()).includes('Max'));
    await page.locator('#historyDetailClose').click();
    assert.equal(await page.locator('#historyDetailDialog').getAttribute('open'), null);
    const options = await page.locator('#historyValueId option').allTextContents();
    assert.ok(options.some(text => text.includes('Redémarrage requis')));
    // Hourly gauge: chart by default (one high/low candle per hour), toggle to the table.
    await page.evaluate(() => loadHistory(true));
    assert.equal(await page.locator('#historyValues svg.history-chart').count(), 1);
    assert.equal(await page.locator('#historyValues .range').count(), 24);
    assert.equal(await page.locator('#historyValues .hit').count(), 24);
    await page.locator('#historyValues .hit').nth(10).click();
    assert.notEqual(await page.locator('#historyDetailDialog').getAttribute('open'), null);
    await page.locator('#historyDetailClose').click();
    await page.locator('.history-view-btn[data-history-view="table"]').click();
    assert.equal(await page.locator('#historyValues svg.history-chart').count(), 0);
    assert.ok((await page.locator('#historyValues').textContent()).includes('Min / Max'));
    // Hourly counter: table exposes the exact 64-bit variation, resets and uncertainty.
    await page.selectOption('#historyValueId', '32');
    await page.evaluate(() => loadHistory(true));
    assert.ok((await page.locator('#historyValues').textContent()).includes('18446744073709551615'));
    assert.ok((await page.locator('#historyValues').textContent()).includes('Ruptures: 1'));
    assert.ok((await page.locator('#historyValues').textContent()).includes('incertaine'));
    // Counter chart renders one bar per hour.
    await page.locator('.history-view-btn[data-history-view="chart"]').click();
    assert.equal(await page.locator('#historyValues .bar').count(), 24);
    await page.evaluate(() => { responseMode = 2; return loadHistory(true); });
    assert.ok((await page.locator('#historyStatus').textContent()).includes('Aucune période'));
    await page.evaluate(() => { responseMode = 1; return loadHistory(); });
    assert.ok((await page.locator('#historyStatus').textContent()).includes('indisponible'));
    assert.equal(await page.locator('#historyRefresh').isDisabled(), false);
    await page.evaluate(() => { responseMode = 0; return loadHistory(); });
    await page.screenshot({ path: '/tmp/flowio-history-desktop.png', fullPage: true });
    await page.setViewportSize({ width: 390, height: 844 });
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
    await page.screenshot({ path: '/tmp/flowio-history-mobile.png', fullPage: true });
    const count = await page.evaluate(() => requests.length);
    await page.waitForTimeout(1100);
    assert.equal(await page.evaluate(() => requests.length), count, 'No background polling');
    console.log('History browser: per-value week table, min/max popup, hourly chart/table toggle, exact uint64, resets, empty/error, responsive layout, no polling OK');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exit(1); });

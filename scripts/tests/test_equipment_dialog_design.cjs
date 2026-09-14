'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const { project, loadDialog, installDialog } = require('./runtime_action_dialog_test_support.cjs');
const entry = loadDialog('PoolDeviceModule', 5, 2305);
const examples = [
  ['Filtration Pump', 14, 18573, 303925, 0, 0],
  ['pH Pump', 15, 148, 632, 49.4, 210.9],
  ['Chlorine Pump', 16, 597, 661, 199.3, 220.4],
  ['Robot', 17, 18835, 74956, 0, 0],
  ['Fill Pump', 18, 46, 13633, 0, 0],
  ['Chlorine Generator', 19, 102, 102, 0, 0],
  ['Lights', 22, 298, 298, 0, 0],
  ['Water Heater', 20, 124, 124, 0, 0]
].map(([name, domainSlot, seconds, totalSeconds, ml, totalMl], value) => ({
  name, domainSlot, value, label: `${name} (pd${value})`, deviceId: `pd${value}`, actualOn: false, controllable: true,
  running: { day_s: seconds, week_s: seconds, month_s: seconds, total_s: totalSeconds },
  injected: { day_ml: ml, week_ml: ml, month_ml: ml, total_ml: totalMl }
}));
async function main() {
  const browser = await chromium.launch({ headless: true,
    ...(process.env.FLOWIO_TEST_BROWSER ? { executablePath: process.env.FLOWIO_TEST_BROWSER } : {}) });
  try {
    const page = await browser.newPage({ viewport: { width: 1660, height: 1088 } });
    await page.setContent('<html lang="fr"><body><div class="status-card"><h3>Équipements</h3></div></body></html>');
    await page.addStyleTag({ content: fs.readFileSync(path.join(project, 'data/webinterface/app-core.css'), 'utf8') });
    await page.evaluate(examples => {
      window.testState = { options: examples, invalidations: 0, refreshes: [] };
      window.fetchOkJson = async () => structuredClone({ options: testState.options });
      window.fetchWithBusyRetry = async () => ({ ok: true, json: async () => ({ ok: true }) });
    }, examples);
    await installDialog(page, entry);
    await page.getByRole('button', { name: entry.displayConfig.actionDialog.buttonText, exact: true }).click();
    const dialog = page.locator('dialog');
    assert.equal(await dialog.locator('tbody tr').count(), 8);
    assert.equal(await dialog.getByRole('switch').count(), 8);
    assert.equal(await dialog.locator('.runtime-management-icon svg').count(), 8);
    assert.equal(await dialog.locator('.runtime-counter-state, .runtime-equipment-state').count(), 0,
      'Equipment identity does not repeat the state shown by the switch');
    const header = await dialog.locator('.runtime-management-header').boundingBox();
    const table = await dialog.locator('table').boundingBox();
    assert(header.y + header.height < table.y, 'Title, count and actions sit above the table');
    assert(await dialog.locator('table').evaluate(element => element.scrollWidth <= element.parentElement.clientWidth),
      'The eight-equipment table fits the desktop popup');
    await dialog.screenshot({ animations: 'disabled', path: '/tmp/flowio-equipment-reference.png' });
    await page.evaluate(async () => {
      testState.options[0].actualOn = true;
      await refreshRuntimeActionDialog(['equipements']);
      const probe = document.createElement('span');
      probe.style.color = 'var(--success)';
      document.body.appendChild(probe);
      window.expectedSwitchColor = getComputedStyle(probe).color;
      probe.remove();
    });
    await page.waitForFunction(() => {
      const track = document.querySelector('dialog input:checked + .md3-track');
      return track && getComputedStyle(track).backgroundColor === expectedSwitchColor;
    });
    assert(await dialog.locator('input:checked + .md3-track + .md3-thumb').evaluate(element =>
      getComputedStyle(element).transitionProperty.includes('transform')), 'The switch keeps its movement animation');
    await dialog.screenshot({ animations: 'disabled', path: '/tmp/flowio-equipment-compact-active.png' });
    await page.evaluate(() => document.documentElement.dataset.theme = 'dark');
    await dialog.screenshot({ animations: 'disabled', path: '/tmp/flowio-equipment-reference-dark.png' });
    await page.evaluate(() => delete document.documentElement.dataset.theme);
    await page.setViewportSize({ width: 390, height: 844 });
    const mobile = await dialog.boundingBox();
    assert(mobile.x >= 0 && mobile.x + mobile.width <= 390);
    for (const button of await dialog.locator('.runtime-management-actions button').all()) {
      const box = await button.boundingBox();
      assert(box.x >= mobile.x && box.x + box.width <= mobile.x + mobile.width, 'Header actions fit on mobile');
    }
    assert(await dialog.locator('.runtime-counter-table-scroll').evaluate(element => element.scrollWidth > element.clientWidth));
    await page.screenshot({ animations: 'disabled', path: '/tmp/flowio-equipment-reference-mobile.png' });
    console.log('Equipment reference design: eight devices, header actions, desktop and mobile layout passed');
  } finally {
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });

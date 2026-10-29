'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const { project, loadDialog, installDialog } = require('./runtime_action_dialog_test_support.cjs');

(async () => {
  const browser = await chromium.launch({ headless: true,
    ...(process.env.FLOWIO_TEST_BROWSER ? { executablePath: process.env.FLOWIO_TEST_BROWSER } : {}) });
  try {
    const page = await browser.newPage();
    await page.setContent('<html lang="fr"><body><div class="status-card"></div></body></html>');
    await page.addStyleTag({ content: fs.readFileSync(path.join(project, 'data/webinterface/app-core.css'), 'utf8') });
    await page.evaluate(() => {
      window.testState = { invalidations: 0, refreshes: [], commands: [], options: [
        { value: 1, label: 'PAC', kind: 3, minimum: 5, maximum: 40, unit: 2,
          setpoint: 28, observedSetpoint: 28, phase: 2, quality: 2, actualOn: true, controllable: true },
        { value: 2, label: 'Pompe', kind: 1, steps: [30, 60, 100], unit: 0,
          setpoint: 60, observedSetpoint: 60, phase: 2, quality: 2, actualOn: true, controllable: true }
      ] };
      window.fetchOkJson = async () => ({ options: structuredClone(testState.options) });
      window.fetchWithBusyRetry = async (_url, request) => {
        testState.commands.push(Object.fromEntries(new URLSearchParams(request.body)));
        return { ok: true, json: async () => ({ ok: true }) };
      };
    });
    const entry = loadDialog('PoolDeviceModule', 5, 2305);
    await installDialog(page, entry);
    await page.locator('.status-card button').click();
    const wrapper = page.locator('.runtime-setpoint').first();
    const input = wrapper.locator('input');
    const apply = wrapper.locator('button');
    const status = wrapper.locator('[aria-live]');
    const commandCount = () => page.evaluate(() => testState.commands.length);

    await input.fill('30');
    await input.press('Enter');
    await input.press('Tab');
    assert.equal(await commandCount(), 0, 'Neither Enter in the field nor blur sends a command');
    assert.equal(await input.inputValue(), '30');
    assert(await apply.evaluate(node => node === document.activeElement));
    await page.evaluate(async () => {
      testState.options[0].observedSetpoint = 29;
      await runtimeActionDialogRefresh.refresh();
    });
    assert.equal(await input.inputValue(), '30', 'Live updates preserve an unfocused draft');
    await apply.press('Space');
    await page.waitForFunction(() => testState.commands.length === 1 && !document.querySelector('.runtime-setpoint input').disabled);
    assert.equal(await page.evaluate(() => testState.commands[0].input), '30');

    for (const invalid of ['', '4', '41']) {
      await input.fill(invalid);
      await apply.click();
      assert.equal(await commandCount(), 1, 'Empty and out-of-range values never reach the API');
      assert.equal(await input.getAttribute('aria-invalid'), 'true');
      assert(await status.isVisible());
      assert((await status.textContent()).length > 0);
    }
    await input.fill('31');
    assert.equal(await input.getAttribute('aria-invalid'), 'false');
    await page.evaluate(async () => {
      Object.assign(testState.options[0], { minimum: 10, maximum: 30, modes: ['Eco', 'Smart'] });
      testState.options[1].steps = [20, 50, 80];
      await runtimeActionDialogRefresh.refresh();
    });
    assert.equal(await input.getAttribute('min'), '10');
    assert.equal(await input.getAttribute('max'), '30');
    assert.equal(await input.inputValue(), '31');
    await apply.click();
    assert.equal(await commandCount(), 1, 'Updated bounds validate the existing draft');
    assert.deepEqual(await wrapper.locator('select option').allTextContents(), ['Eco', 'Smart']);
    const discrete = page.locator('select.runtime-setpoint-input');
    assert.deepEqual(await discrete.locator('option').allTextContents(), ['20', '50', '80']);
    await discrete.selectOption('50');
    await discrete.press('Tab');
    assert.equal(await commandCount(), 1);
    await page.evaluate(async () => {
      testState.options[1].steps = [20, 80];
      await runtimeActionDialogRefresh.refresh();
    });
    await page.locator('.runtime-setpoint').nth(1).locator('button').click();
    assert.equal(await commandCount(), 1, 'A removed discrete option cannot be submitted');
    await page.evaluate(async () => {
      testState.options[0].actualOn = false;
      testState.options[0].modes = [];
      await runtimeActionDialogRefresh.refresh();
    });
    assert(await input.isDisabled());
    assert(await apply.isHidden());
    assert(await wrapper.locator('select').isHidden());
    console.log('Explicit setpoint apply: keyboard, drafts, validation, live capabilities and stopped devices passed');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });

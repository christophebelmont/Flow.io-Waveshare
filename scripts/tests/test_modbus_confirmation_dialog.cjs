const assert = require('node:assert/strict');
const { chromium } = require('playwright');
const { loadDialog, installDialog } = require('./runtime_action_dialog_test_support.cjs');

(async () => {
  const browser = await chromium.launch({ headless: true,
    ...(process.env.FLOWIO_TEST_BROWSER ? { executablePath: process.env.FLOWIO_TEST_BROWSER } : {}) });
  try {
    const page = await browser.newPage();
    await page.setContent('<html lang="fr"><body><div class="status-card"></div></body></html>');
    await page.evaluate(() => {
      window.testState = { invalidations: 0, refreshes: [], refuse: false, deferSnapshot: false, commands: [], option: {
        value: 8, label: 'PAC', name: 'PAC', deviceId: 'pd8', kind: 3, unit: 2,
        minimum: 0, maximum: 60, setpoint: 28, observedSetpoint: 28, acceptedSetpoint: 28,
        phase: 2, quality: 2, actualOn: true, controllable: true
      }};
      window.fetchOkJson = async () => ({ options: [structuredClone(testState.option)] });
      window.fetchWithBusyRetry = async (_url, request) => {
        const params = new URLSearchParams(request.body);
        testState.commands.push(Number(params.get('input')));
        await new Promise(resolve => { testState.release = resolve; });
        if (!testState.refuse && !testState.deferSnapshot) Object.assign(testState.option, {
          setpoint: Number(params.get('input')), acceptedSetpoint: Number(params.get('input')), phase: 5, quality: 2
        });
        return { ok: !testState.refuse, json: async () => ({ ok: !testState.refuse, error: 'Commande refusée' }) };
      };
    });
    const entry = loadDialog('PoolDeviceModule', 5, 2305);
    await installDialog(page, entry);
    await page.getByRole('button', { name: entry.displayConfig.actionDialog.buttonText, exact: true }).click();
    const status = page.locator('.runtime-setpoint [aria-live]');
    const input = page.locator('.runtime-setpoint input');
    const field = page.locator('.runtime-setpoint-field');
    const apply = page.locator('.runtime-setpoint-apply');
    assert.match(await status.textContent(), /Confirmé/);
    await input.fill('30'); await input.press('Tab');
    assert.equal(await input.inputValue(), '30');
    assert.equal(await apply.evaluate(button => button === document.activeElement), true);
    assert.match(await status.textContent(), /Modification non appliquée/);
    assert.equal(await page.evaluate(() => typeof testState.release), 'undefined');
    await apply.press('Enter');
    assert.match(await status.textContent(), /Confirmation en cours/);
    assert.equal(await field.locator('.runtime-setpoint-prefix').textContent(), '28→');
    await page.waitForFunction(() => typeof testState.release === 'function');
    await page.evaluate(() => testState.release());
    await page.waitForFunction(() => !document.querySelector('.runtime-setpoint input').disabled);
    assert.match(await status.textContent(), /Commande acceptée · Application en attente/);
    assert.equal(await status.evaluate(node => node.classList.contains('visually-hidden')), false);
    assert.equal(await input.inputValue(), '30');
    // Valid but different readings never replace an accepted pending target.
    await page.evaluate(async () => {
      Object.assign(testState.option, { observedSetpoint: 29, phase: 5, quality: 2 });
      await runtimeActionDialogRefresh.refresh();
    });
    assert.equal(await input.inputValue(), '30');
    assert.match(await field.getAttribute('class'), /is-pending/);
    // A replacement is possible before the first target has converged.
    await page.evaluate(() => { testState.release = null; });
    await input.fill('32'); await input.press('Tab');
    assert.equal(await page.evaluate(() => testState.commands.length), 1);
    await apply.click();
    await page.waitForFunction(() => typeof testState.release === 'function');
    await page.evaluate(() => testState.release());
    await page.waitForFunction(() => !document.querySelector('.runtime-setpoint input').disabled);
    assert.equal(await input.inputValue(), '32');
    assert.match(await status.textContent(), /Application en attente/);
    await page.evaluate(async () => {
      Object.assign(testState.option, { observedSetpoint: 32, phase: 2, quality: 2 });
      await runtimeActionDialogRefresh.refresh();
    });
    assert.match(await field.getAttribute('class'), /is-confirmed/);
    assert.match(await status.textContent(), /Confirmé/);
    assert.equal(await input.inputValue(), '32');
    assert.equal(await status.evaluate(node => node.classList.contains('visually-hidden')), true);
    // The device's normalized accepted target explains a different final value.
    await page.evaluate(async () => {
      testState.option.setpoint = 32.4;
      await runtimeActionDialogRefresh.refresh();
    });
    assert.match(await status.textContent(), /Demandé : 32.4 °C · Retenu : 32 °C/);
    // HTTP refusal must not borrow the previous reading's green confirmation.
    await page.evaluate(() => { testState.refuse = true; testState.release = null; });
    await input.fill('33'); await input.press('Tab');
    await apply.click();
    await page.waitForFunction(() => typeof testState.release === 'function');
    await page.evaluate(() => testState.release());
    await page.waitForFunction(() => !document.querySelector('.runtime-setpoint input').disabled);
    assert(!(await status.textContent()).includes('Confirmation en cours'));
    assert.match(await status.textContent(), /Commande refusée/);
    assert.match(await field.getAttribute('class'), /is-error/);
    assert.equal(await input.inputValue(), '33');
    await page.evaluate(async () => { await runtimeActionDialogRefresh.refresh(); });
    assert.match(await field.getAttribute('class'), /is-error/);
    assert.equal(await input.inputValue(), '33');
    assert.match(await page.locator('dialog [role="status"]').textContent(), /Commande refusée/);
    // HTTP success preceding its runtime snapshot must also retain the request.
    await page.evaluate(() => { testState.refuse = false; testState.deferSnapshot = true; testState.release = null; });
    await input.fill('34'); await apply.click();
    await page.waitForFunction(() => typeof testState.release === 'function');
    await page.evaluate(() => testState.release());
    await page.waitForFunction(() => !document.querySelector('.runtime-setpoint input').disabled);
    await page.evaluate(async () => { await runtimeActionDialogRefresh.refresh(); });
    assert.equal(await input.inputValue(), '34');
    assert.match(await field.getAttribute('class'), /is-pending/);
    await page.evaluate(async () => {
      Object.assign(testState.option, { setpoint: 34, phase: 3, quality: 0, observedSetpoint: null });
      await runtimeActionDialogRefresh.refresh();
    });
    assert.equal(await input.inputValue(), '34');
    assert.match(await status.textContent(), /Échec de la commande/);
    assert.match(await field.getAttribute('class'), /is-error/);
    // Reopening reconstructs a pump's delayed command entirely from the API.
    await page.evaluate(() => {
      document.querySelector('dialog').close();
      Object.assign(testState.option, { unit: 3, minimum: 1200, maximum: 2900,
        setpoint: 1800, acceptedSetpoint: 1800, observedSetpoint: 2900, phase: 5, quality: 2 });
    });
    await page.getByRole('button', { name: entry.displayConfig.actionDialog.buttonText, exact: true }).click();
    assert.equal(await input.inputValue(), '1800');
    assert.equal(await field.locator('.runtime-setpoint-prefix').textContent(), '2900→');
    for (let i = 0; i < 5; ++i) {
      await page.evaluate(async () => { await runtimeActionDialogRefresh.refresh(); });
      assert.equal(await input.inputValue(), '1800');
      assert.match(await field.getAttribute('class'), /is-pending/);
      assert.match(await status.textContent(), /Application en attente/);
    }
    console.log('MODBUS confirmation dialog tests passed');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });

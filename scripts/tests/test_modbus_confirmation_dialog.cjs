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
      window.testState = { invalidations: 0, refreshes: [], refuse: false, option: {
        value: 8, label: 'PAC', name: 'PAC', deviceId: 'pd8', kind: 3, unit: 2,
        minimum: 0, maximum: 60, setpoint: 28, observedSetpoint: 28,
        phase: 2, quality: 2, actualOn: false, controllable: true
      }};
      window.fetchOkJson = async () => ({ options: [structuredClone(testState.option)] });
      window.fetchWithBusyRetry = async (_url, request) => {
        const params = new URLSearchParams(request.body);
        await new Promise(resolve => { testState.release = resolve; });
        if (!testState.refuse) Object.assign(testState.option, {
          setpoint: Number(params.get('input')), phase: 5, quality: 3
        });
        return { ok: !testState.refuse, json: async () => ({ ok: !testState.refuse, error: 'Commande refusée' }) };
      };
    });
    const entry = loadDialog('PoolDeviceModule', 5, 2305);
    await installDialog(page, entry);
    await page.getByRole('button', { name: entry.displayConfig.actionDialog.buttonText, exact: true }).click();
    const status = page.locator('.runtime-setpoint [aria-live]');
    const input = page.locator('.runtime-setpoint input');
    assert.match(await status.textContent(), /28 °C · Confirmé/);
    await input.fill('30'); await input.press('Tab');
    assert.match(await status.textContent(), /30 °C · Confirmation en cours/);
    assert.match(await status.textContent(), /Dernière lecture : 28 °C/);
    await page.waitForFunction(() => typeof testState.release === 'function');
    await page.evaluate(() => testState.release());
    await page.waitForFunction(() => !document.querySelector('.runtime-setpoint input').disabled);
    assert.match(await status.textContent(), /30 °C · Confirmation en cours/);
    assert(!(await status.textContent()).includes('Périmé'));
    // Readback is authoritative even when it differs from the requested value.
    await page.evaluate(async () => {
      Object.assign(testState.option, { observedSetpoint: 29, phase: 2, quality: 2 });
      await runtimeActionDialogRefresh.refresh();
    });
    assert.match(await status.textContent(), /29 °C · Confirmé/);
    await page.evaluate(async () => {
      Object.assign(testState.option, { phase: 3, quality: 0, observedSetpoint: null });
      await runtimeActionDialogRefresh.refresh();
    });
    assert.match(await status.textContent(), /Échec de la commande/);
    await page.evaluate(() => { testState.refuse = true; testState.release = null; });
    await input.fill('31'); await input.press('Tab');
    await page.waitForFunction(() => typeof testState.release === 'function');
    await page.evaluate(() => testState.release());
    await page.waitForFunction(() => !document.querySelector('.runtime-setpoint input').disabled);
    assert(!(await status.textContent()).includes('Confirmation en cours'));
    assert.match(await page.locator('dialog [role="status"]').textContent(), /Commande refusée/);
    console.log('MODBUS confirmation dialog tests passed');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });

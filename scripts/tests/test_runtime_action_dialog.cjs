// Run with Node.js and Playwright installed. Set FLOWIO_TEST_BROWSER to use an installed Chromium browser.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

const { project, loadDialog, installDialog } = require('./runtime_action_dialog_test_support.cjs');
const entry = loadDialog('PoolDeviceModule', 5, 2305);
const config = entry.displayConfig.actionDialog;
assert.equal(entry.actions.find(action => action.id === 'set_device').command, 'poollogic.device.write',
  'Popup toggles use the business command dispatcher');

async function main() {
  const browser = await chromium.launch({ headless: true,
    ...(process.env.FLOWIO_TEST_BROWSER ? { executablePath: process.env.FLOWIO_TEST_BROWSER } : {}) });
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
    await page.setContent('<html lang="fr"><body><div class="status-card"><h3>Équipements</h3></div></body></html>');
    await page.addStyleTag({ content: fs.readFileSync(path.join(project, 'data/webinterface/app-core.css'), 'utf8') });
    await page.evaluate(() => {
      window.testState = { options: [
        { value: 0, label: 'Filtration (pd0)', name: 'Filtration', deviceId: 'pd0', actualOn: true, controllable: true,
          running: { day_s: 3600, week_s: 90000, month_s: 360000, total_s: 3600000 },
          injected: { day_ml: 0, week_ml: 0, month_ml: 0, total_ml: 0 } },
        { value: 8, label: 'COMP01 désactivé (pd8)', name: 'COMP01 désactivé', deviceId: 'pd8', actualOn: null, controllable: false, running: null, injected: null },
        { value: 15, label: 'COMP08 (pd15)', name: 'COMP08', deviceId: 'pd15', actualOn: false, controllable: true,
          running: { day_s: 3661, week_s: 7322, month_s: 10983, total_s: 14644 },
          injected: { day_ml: 125.5, week_ml: 251, month_ml: 376.5, total_ml: 502 } }
        ], requests: [], refreshes: [], invalidations: 0, failList: false,
        failCommand: false, failAfterCommand: false, delay: 0 };
      window.fetchOkJson = async url => {
        assertOptionsUrl(url);
        if (testState.failList) throw new Error('Service indisponible');
        return structuredClone({ options: testState.options });
      };
      window.assertOptionsUrl = url => { if (url !== '/api/runtime/pooldevice_options') throw new Error('Unexpected options source'); };
      window.fetchWithBusyRetry = async (url, request) => {
        if (url !== '/api/runtime/action') throw new Error('Unexpected action route');
        testState.requests.push(Object.fromEntries(new URLSearchParams(request.body)));
        if (testState.delay) await new Promise(resolve => setTimeout(resolve, testState.delay));
        if (!testState.failCommand) {
          const params = new URLSearchParams(request.body);
          const input = params.get('input');
          testState.options.forEach(option => {
            if (params.get('action_id') === 'set_device') {
              if (option.value === Number(params.get('target'))) option.actualOn = input === 'true';
              return;
            }
            if (input !== null && option.value !== Number(input)) return;
            for (const period of ['day', 'week', 'month']) {
              if (option.running) option.running[period + '_s'] = 0;
              if (option.injected) option.injected[period + '_ml'] = 0;
            }
          });
          if (testState.failAfterCommand) testState.failList = true;
        }
        return { ok: !testState.failCommand, json: async () => testState.failCommand
          ? { ok: false, error: 'Équipement inconnu' } : { ok: true } };
      };

    });
    await installDialog(page, entry);

    const nativeDialogs = [];
    page.on('dialog', async dialog => {
      nativeDialogs.push(dialog.message());
      await dialog.dismiss();
    });
    const open = () => page.getByRole('button', { name: config.buttonText, exact: true }).click();
    const close = () => page.getByRole('button', { name: 'Fermer', exact: true }).click();
    const resetAll = page.getByRole('button', { name: config.allButtonText, exact: true });
    const resetDevice = label => page.getByRole('button', { name: config.rowButtonText + ' — ' + label, exact: true });
    const row = label => page.locator('tbody tr').filter({ has: page.getByRole('rowheader', { name: label, exact: true }) });
    const feedback = page.locator('dialog [role="status"]');
    const confirm = page.getByRole('button', { name: config.confirmButtonText, exact: true });
    const confirmReset = async label => { await resetDevice(label).click(); await confirm.click(); };

    await open();
    assert.equal(await page.locator('tbody tr').count(), 3, 'Every registered device has a row');
    assert.equal(await page.locator('.runtime-counter-state.is-running').count(), 1, 'Running state comes from firmware data');
    assert.equal(await page.locator('.runtime-counter-details').evaluate(element => element.open), false, 'Reset explanation starts collapsed');
    assert(!(await page.locator('dialog').textContent()).includes('flow.io'), 'The popup has no flow.io logo');
    assert.equal(await resetDevice('COMP08 (pd15)').textContent(), '↺', 'Row actions use compact glyphs');
    assert.equal(await page.locator('thead th').count(), 7, 'Device, On/Off, four periods and action columns');
    assert.equal(config.buttonText, 'Gérer les équipements');
    assert(await row('Filtration (pd0)').getByRole('switch').isChecked());
    assert(!(await row('COMP08 (pd15)').getByRole('switch').isChecked()));
    assert(await row('COMP01 désactivé (pd8)').getByRole('switch').isDisabled());
    assert.deepEqual(await row('COMP08 (pd15)').locator('.runtime-counter-duration').allTextContents(),
      ['01:01:01', '02:02:02', '03:03:03', '04:04:04']);
    assert.deepEqual(await row('COMP08 (pd15)').locator('.runtime-counter-volume').allTextContents(),
      ['125,5 mL', '251,0 mL', '376,5 mL', '502,0 mL']);
    assert.equal(await row('Filtration (pd0)').locator('.runtime-counter-duration').nth(1).textContent(), '25:00:00');
    assert.equal(await row('Filtration (pd0)').locator('.runtime-counter-volume').first().textContent(), '0,0 mL');
    assert.deepEqual(await row('COMP01 désactivé (pd8)').locator('.runtime-counter-duration').allTextContents(), ['—', '—', '—', '—']);
    if (process.env.FLOWIO_TEST_SCREENSHOT) await page.screenshot({ path: process.env.FLOWIO_TEST_SCREENSHOT });
    await page.setViewportSize({ width: 390, height: 844 });
    const box = await page.locator('dialog').boundingBox();
    assert(box.x >= 0 && box.x + box.width <= 390, 'Dialog fits a mobile viewport');
    assert(await page.locator('.runtime-counter-table-scroll').evaluate(element => element.scrollWidth > element.clientWidth),
      'The table scrolls horizontally on mobile');
    if (process.env.FLOWIO_TEST_SCREENSHOT) await page.screenshot({ path: process.env.FLOWIO_TEST_SCREENSHOT.replace('.png', '-mobile.png') });
    await page.setViewportSize({ width: 1280, height: 900 });

    await resetDevice('COMP08 (pd15)').click();
    assert.equal(await page.locator('.runtime-counter-confirmation').count(), 1, 'Confirmation is embedded in the table');
    assert.equal(await row('COMP08 (pd15)').evaluate(element => element.nextElementSibling.className), 'runtime-counter-confirmation');
    assert.match(await page.locator('.runtime-counter-confirmation').textContent(), /COMP08.*Totaux conservés/);
    if (process.env.FLOWIO_TEST_SCREENSHOT) await page.screenshot({ path: process.env.FLOWIO_TEST_SCREENSHOT.replace('.png', '-confirmation.png') });
    await page.getByRole('button', { name: 'Annuler', exact: true }).click();
    assert.equal(await page.evaluate(() => testState.requests.length), 0, 'Cancelled confirmation produces no reset');
    assert.equal(await page.locator('.runtime-counter-confirmation').count(), 0);
    await confirmReset('COMP08 (pd15)');
    assert.match(await feedback.textContent(), /COMP08.*remis à zéro/);
    assert.equal(await row('COMP08 (pd15)').evaluate(element => element.classList.contains('is-updated')), true, 'Updated rows get brief visual feedback');
    assert.deepEqual(await page.evaluate(() => testState.requests[0]), { runtime_id: '2305', action_id: 'reset_uptime', input: '15' });
    assert.deepEqual(await page.evaluate(() => testState.refreshes), ['equipements', 'alarm', 'overview']);
    assert.equal(await page.evaluate(() => testState.invalidations), 1);
    assert.deepEqual(await row('COMP08 (pd15)').locator('.runtime-counter-duration').allTextContents(),
      ['00:00:00', '00:00:00', '00:00:00', '04:04:04'], 'Post-reset reads preserve lifetime running time');
    assert.deepEqual(await row('COMP08 (pd15)').locator('.runtime-counter-volume').allTextContents(),
      ['0,0 mL', '0,0 mL', '0,0 mL', '502,0 mL'], 'Post-reset reads preserve lifetime volume');

    await confirmReset('COMP01 désactivé (pd8)');
    assert.equal(await page.evaluate(() => testState.requests[1].input), '8', 'Disabled, off-card devices are actionable');
    await page.evaluate(() => { testState.delay = 1000; });
    await resetAll.click();
    assert.match(await page.locator('.runtime-counter-confirmation').textContent(), /Tous les équipements/);
    await confirm.click();
    assert(await resetDevice('Filtration (pd0)').isDisabled(), 'All row actions are frozen during a command');
    assert(await row('COMP08 (pd15)').getByRole('switch').isDisabled(), 'Switches are frozen during resets');
    await page.keyboard.press('Escape');
    assert(await page.locator('dialog').isVisible(), 'Escape cannot dismiss an in-progress command');
    await page.waitForFunction(() => document.querySelector('dialog .btn-row button').disabled === false);
    assert.deepEqual(await page.evaluate(() => testState.requests[2]), { runtime_id: '2305', action_id: 'reset_uptime_all' });

    await page.evaluate(() => { testState.delay = 0; testState.failCommand = true; });
    await confirmReset('Filtration (pd0)');
    assert.match(await feedback.textContent(), /Équipement inconnu/);
    assert(await resetDevice('Filtration (pd0)').isEnabled(), 'A rejected command can be retried');

    const switchDevice = row('COMP08 (pd15)').getByRole('switch');
    await switchDevice.click();
    assert.match(await feedback.textContent(), /Équipement inconnu/);
    assert(!(await switchDevice.isChecked()), 'A refused start keeps the confirmed Off state');
    assert(await switchDevice.isEnabled(), 'A refused switch command can be retried');
    await page.evaluate(() => { testState.failCommand = false; testState.delay = 700; });
    await switchDevice.click();
    assert(await switchDevice.isDisabled(), 'Switch command locks all actions');
    assert(!(await switchDevice.isChecked()), 'Pending start does not display an optimistic On state');
    assert(await resetAll.isDisabled());
    await page.waitForFunction(() => document.querySelector('dialog .btn-row button').disabled === false);
    assert(await switchDevice.isChecked(), 'The actual state is read again after start');
    assert.deepEqual(await page.evaluate(() => testState.requests[5]),
      { runtime_id: '2305', action_id: 'set_device', input: 'true', target: '15' }, 'Off-card switch targets its actual slot');
    assert.deepEqual(await row('COMP08 (pd15)').locator('.runtime-counter-duration').allTextContents(),
      ['00:00:00', '00:00:00', '00:00:00', '04:04:04'], 'Switching preserves counters');
    await page.evaluate(() => { testState.delay = 0; });
    await switchDevice.click();
    await page.waitForFunction(() => document.querySelector('dialog .btn-row button').disabled === false);
    assert(!(await switchDevice.isChecked()), 'The toggle supports stopping');
    assert.equal(await page.evaluate(() => testState.requests[6].input), 'false');
    await close();

    await page.evaluate(() => { testState.failList = true; });
    await open();
    assert.match(await feedback.textContent(), /Service indisponible/);
    assert(await resetAll.isDisabled(), 'List failures cannot trigger resets');
    await close();

    await page.evaluate(() => { testState.failList = false; testState.failCommand = false; testState.failAfterCommand = true; });
    await open();
    await confirmReset('Filtration (pd0)');
    assert.match(await feedback.textContent(), /remis à zéro.*Service indisponible/);
    assert.equal(await page.locator('tbody tr').count(), 0, 'Stale metrics are removed after a failed post-reset read');
    assert(await resetAll.isDisabled());
    await close();

    await page.evaluate(() => { testState.failList = false; testState.options = []; });
    await open();
    assert.match(await feedback.textContent(), /Aucun équipement/);
    assert(await resetAll.isDisabled(), 'Empty lists cannot trigger resets');
    await close();
    assert.equal(await page.evaluate(() => testState.requests.length), 8, 'Cancel/list failures produce no extra commands');
    assert.deepEqual(nativeDialogs, [], 'Confirmation does not open a separate browser popup');
    console.log('Runtime action dialog: all browser checks passed');
  } finally {
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });

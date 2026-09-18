// Uses real Runtime UI controls and executor with firmware I/O replaced by alarm fixtures.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const { project, loadDialog, installDialog } = require('./runtime_action_dialog_test_support.cjs');
const entry = loadDialog('AlarmModule', 2, 902);
const config = entry.displayConfig.actionDialog;

async function main() {
  const browser = await chromium.launch({ headless: true,
    ...(process.env.FLOWIO_TEST_BROWSER ? { executablePath: process.env.FLOWIO_TEST_BROWSER } : {}) });
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
    await page.setContent('<html lang="fr"><body><div class="status-card"><h3>Alarmes</h3></div></body></html>');
    await page.addStyleTag({ content: fs.readFileSync(path.join(project, 'data/webinterface/app-core.css'), 'utf8') });
    await page.evaluate(() => {
      window.testState = { options: [
        { value: 107, label: 'Pompe pH', triggeredAt: 1789375338, condition: 0, latchState: 1, resettable: true, automatic: false },
        { value: 103, label: 'Débit filtration', triggeredAt: 1789375338, condition: 1, latchState: 1, resettable: false, automatic: false },
        { value: 211, label: 'Sonde indisponible', triggeredAt: null, condition: 2, latchState: 1, resettable: false, automatic: false },
        { value: 101, label: 'Cuve pH', triggeredAt: null, condition: 0, latchState: 2, resettable: false, automatic: true },
        { value: 201, label: 'Pompe chlore', triggeredAt: 1789375338, condition: 0, latchState: 1, resettable: true, automatic: false }
      ], requests: [], invalidations: 0, refreshes: [], failList: false, failCommand: false, delay: 0 };
      window.fetchOkJson = async url => {
        if (url !== '/api/runtime/alarm_options') throw new Error('Wrong source');
        if (testState.failList) throw new Error('Service indisponible');
        return structuredClone({ options: testState.options });
      };
      window.fetchWithBusyRetry = async (url, request) => {
        if (url !== '/api/runtime/action') throw new Error('Wrong action route');
        const args = Object.fromEntries(new URLSearchParams(request.body));
        testState.requests.push(args);
        if (testState.delay) await new Promise(resolve => setTimeout(resolve, testState.delay));
        if (testState.failCommand) return { ok: false, json: async () => ({ ok: false, error: 'Condition encore active' }) };
        testState.options.forEach(alarm => {
          if ((args.input === undefined || alarm.value === Number(args.input)) && alarm.resettable) {
            alarm.latchState = 0;
            alarm.resettable = false;
          }
        });
        return { ok: true, json: async () => ({ ok: true }) };
      };
    });
    await installDialog(page, entry);
    await page.getByRole('button', { name: config.buttonText }).click();
    const row = name => page.locator('tbody tr').filter({ has: page.getByRole('rowheader', { name, exact: true }) });
    const ack = name => page.getByRole('button', { name: config.rowButtonText + ' — ' + name, exact: true });
    const confirm = page.getByRole('button', { name: config.confirmButtonText, exact: true });
    const all = page.getByRole('button', { name: config.allButtonText, exact: true });
    assert.equal(await page.locator('tbody tr').count(), 5);
    assert.equal(await page.locator('thead th').count(), 5);
    assert.match(await page.locator('.runtime-counter-count').textContent(), /5 alarmes · 2 à acquitter/);
    assert.match(await row('Pompe pH').textContent(), /Inactive.*Mémorisé/);
    assert.equal(await row('Pompe pH').locator('td').first().locator('div').count(), 2, 'Date and time are shown separately');
    assert.equal(await row('Sonde indisponible').locator('td').first().textContent(), '—', 'Invalid clock dates stay unavailable');
    assert(await ack('Pompe pH').isEnabled());
    assert(await ack('Débit filtration').isDisabled());
    assert(await ack('Sonde indisponible').isDisabled());
    assert.match(await row('Cuve pH').textContent(), /Non utilisé.*Automatique/);
    assert.equal(await row('Cuve pH').getByRole('button').count(), 0, 'Automatic alarms have no acknowledge action');
    if (process.env.FLOWIO_TEST_SCREENSHOT) await page.screenshot({ path: process.env.FLOWIO_TEST_SCREENSHOT });
    await page.setViewportSize({ width: 390, height: 844 });
    assert(await page.locator('.runtime-counter-table-scroll').evaluate(element => element.scrollWidth > element.clientWidth));
    const box = await page.locator('dialog').boundingBox();
    assert(box.x >= 0 && box.x + box.width <= 390);
    await page.setViewportSize({ width: 1280, height: 900 });

    await ack('Pompe pH').click();
    await page.getByRole('button', { name: 'Annuler', exact: true }).click();
    assert.equal(await page.evaluate(() => testState.requests.length), 0);
    await ack('Pompe pH').click();
    await confirm.click();
    assert.deepEqual(await page.evaluate(() => testState.requests[0]), { runtime_id: '902', action_id: 'acknowledge', input: '107' }, 'Use AlarmId, never card position');
    assert.match(await row('Pompe pH').textContent(), /Inactive.*Libre/);
    assert(await ack('Pompe pH').isDisabled());
    assert.equal(await page.evaluate(() => testState.options[0].triggeredAt), 1789375338, 'Acknowledgement retains last trigger date');
    assert.deepEqual(await page.evaluate(() => testState.refreshes), ['alarm', 'overview']);

    await page.evaluate(() => { testState.failCommand = true; });
    await ack('Pompe chlore').click();
    await confirm.click();
    assert.match(await page.locator('[role="status"]').textContent(), /Condition encore active/);
    assert(await confirm.isEnabled(), 'Firmware rejection remains visible and retryable');
    await page.getByRole('button', { name: 'Annuler', exact: true }).click();
    await page.evaluate(() => { testState.failCommand = false; testState.delay = 1000; });
    await all.click();
    await confirm.click();
    assert(await ack('Pompe chlore').isDisabled());
    await page.keyboard.press('Escape');
    assert(await page.locator('dialog').isVisible());
    await page.waitForFunction(() => window.testState.options.every(alarm => !alarm.resettable));
    assert.deepEqual(await page.evaluate(() => testState.requests[2]), { runtime_id: '902', action_id: 'acknowledge_all' });
    assert.match(await row('Débit filtration').textContent(), /Active.*Mémorisé/, 'Blocked latches are kept');
    assert.match(await row('Sonde indisponible').textContent(), /Inconnue.*Mémorisé/);
    assert(await all.isDisabled(), 'Global action disables when no alarm is eligible');
    await page.getByRole('button', { name: 'Fermer', exact: true }).click();

    // Check the actual tile builder and card footer rather than a copy of their markup.
    const app = fs.readFileSync(path.join(project, 'data/webinterface/app.js'), 'utf8');
    const tile = app.slice(app.indexOf('    function buildDashboardAlarmTile('), app.indexOf('    function buildRuntimeAlarmGrid('));
    const footer = app.slice(app.indexOf('    function appendRuntimeCardActions('), app.indexOf('    function buildPoolMeasureCards('));
    await page.evaluate(({ tile, footer }) => {
      window.decorateDashboardAlarmTile = () => {};
      window.dashboardAlarmStateText = () => 'À acquitter';
      const script = document.createElement('script');
      script.textContent = tile + footer + '\nconst alarmCard = document.createElement("div"); alarmCard.id="testAlarmCard"; alarmCard.className="status-card"; alarmCard.appendChild(buildDashboardAlarmTile({label:"Pompe pH",conditionValue:false,latchValue:true,resettable:true,inputValue:107,actionBinding:{entry:testEntry,action:testEntry.actions[0]}})); appendRuntimeCardActions(alarmCard,[testEntry]); document.body.appendChild(alarmCard);';
      document.body.appendChild(script);
    }, { tile, footer });
    assert.equal(await page.locator('#testAlarmCard .status-alarm-slot').evaluate(element => element.tagName), 'DIV');
    const requestCount = await page.evaluate(() => testState.requests.length);
    await page.locator('#testAlarmCard .status-alarm-slot').click();
    assert.equal(await page.evaluate(() => testState.requests.length), requestCount, 'Clicking an alarm tile cannot acknowledge it');
    assert.equal(await page.locator('#testAlarmCard .runtime-card-actions button').count(), 1);
    assert.equal(await page.locator('#testAlarmCard .runtime-card-actions').evaluate(element => getComputedStyle(element).justifyContent), 'flex-end');
    console.log('Alarm popup: all browser checks passed');
  } finally {
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });

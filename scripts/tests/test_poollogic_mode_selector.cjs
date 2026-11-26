const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '../..');
const app = fs.readFileSync(path.join(root, 'data/webinterface/app.js'), 'utf8');
const start = app.indexOf('    const poolOperatingModes =');
const end = app.indexOf('    function formatDashboardGaugeRange', start);
assert(start > 0 && end > start);
const manifest = JSON.parse(fs.readFileSync(path.join(root, 'src/Modules/PoolLogicModule/text/runtimeui.json'), 'utf8'));
assert.equal(manifest.values.length, 1);
const entry = { ...manifest.values[0], id: 2401 };
assert.equal(entry.key, 'pool.operating_mode');
assert.deepEqual(entry.enum, { 0: 'Manu', 1: 'Auto', 2: 'Hiver' });
const buttons = ['auto', 'manual', 'winter'].map(mode => ({
  dataset: { mode }, attributes: {}, selected: false,
  classList: { toggle(_name, selected) { buttons.find(button => button.dataset.mode === mode).selected = selected; } },
  setAttribute(name, value) { this.attributes[name] = value; }
}));
const calls = [];
const context = vm.createContext({
  dashModeButtons: buttons, runtimeActionBusyKey: '',
  poolMeasureDomainState: { mode: { entries: [entry], values: [{ id: 2401, value: 0 }] } },
  executeRuntimeAction: async (entry, action, value) => calls.push({ id: entry.id, command: action.command, value })
});
vm.runInContext(app.slice(start, end), context);
(async () => {
  for (const [mode, name] of [[0, 'manual'], [1, 'auto'], [2, 'winter']]) {
    context.poolMeasureDomainState.mode.values[0].value = mode;
    context.renderDashboardModeControl();
    assert.equal(buttons.find(button => button.selected).dataset.mode, name);
    assert(buttons.every(button => !button.disabled));
  }
  await context.applyDashboardMode('winter');
  assert.equal(calls.length, 0); // Selecting the current mode is a no-op.
  await context.applyDashboardMode('auto');
  assert.deepEqual(calls, [{ id: 2401, command: 'poollogic.operating_mode.set', value: 1 }]);
  await context.applyDashboardMode('invalid');
  assert.equal(calls.length, 1);
  context.runtimeActionBusyKey = '2401:set';
  context.renderDashboardModeControl();
  assert(buttons.every(button => button.disabled));
  context.runtimeActionBusyKey = '';
  context.poolMeasureDomainState.mode.values[0].value = null;
  context.renderDashboardModeControl();
  assert(buttons.every(button => button.disabled && !button.selected));
  console.log('PoolLogic selector: three states, one command per change, no intermediate mode.');
})().catch(error => { console.error(error); process.exitCode = 1; });

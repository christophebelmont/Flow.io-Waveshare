const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { loadFunctions } = require('./app_function_test_support.cjs');
const source = fs.readFileSync(path.resolve(__dirname, '../../data/webinterface/app.js'), 'utf8');

const modules = {
  'io/input/i00': { mode: 1, i00_name: 'Compteur piscine' },
  'io/input/i01': { mode: 1, i01_name: '  Débit  ' },
  'io/input/i02': { mode: 0, i02_name: 'Marche' },
  'io/input/i03': { mode: 1, i03_name: '' }
};
const context = vm.createContext({
  derivedValueDigitalMax: 3,
  derivedEntityNames: {},
  derivedCounterInputs: [],
  fetchJsonResponse: async (url) => {
    const name = decodeURIComponent(String(url).replace('/api/flowcfg/module?name=', ''));
    const data = modules[name];
    return { res: { ok: !!data }, data: { ok: !!data, data: data || {} } };
  }
});
loadFunctions(source, context, ['loadDerivedCounterInputs']);

(async () => {
  await context.loadDerivedCounterInputs();
  assert.deepEqual([...context.derivedCounterInputs], [0, 1, 3], 'Every counter-mode input is exposed');
  assert.equal(context.derivedEntityNames.i00, 'Compteur piscine', 'Name read from iNN_name');
  assert.equal(context.derivedEntityNames.i01, 'Débit', 'Name is trimmed');
  assert.equal(context.derivedEntityNames.i02, undefined, 'State-mode input name is ignored');
  assert.equal(context.derivedEntityNames.i03, undefined, 'Empty name is ignored');
  console.log('Derived counters: names loaded from iNN_name, state/empty inputs excluded OK');
})().catch(error => { console.error(error); process.exitCode = 1; });

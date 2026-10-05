const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.resolve(__dirname, '../../data/webinterface/app.js'), 'utf8');
function extract(name) {
  const expression = new RegExp('^    (?:async )?function ' + name + '\\(', 'm');
  const start = source.search(expression);
  assert(start >= 0, name);
  const rest = source.slice(start + 1);
  const end = rest.search(/^    (?:async )?function /m);
  assert(end >= 0, name);
  return source.slice(start, start + 1 + end);
}
const requests = [];
let renders = 0;
let savedName = 'Débit piscine';
const context = vm.createContext({
  cfgTreeNodeTextNames: {}, cfgTreeNodeTextNamePending: new Set(),
  nettoyerNomFlowCfg: value => value.trim(),
  cfgStorePathFromDisplayPath: value => value,
  renderFlowCfgTree: () => { ++renders; },
  fetchWithBusyRetry: async url => {
    requests.push(url);
    return { ok: true, json: async () => ({ ok: true, data: { name: savedName, a00_name: 'Pression' } }) };
  }
});
for (const name of ['cfgTreeNodeRefInfo', 'fetchCfgTreeNodeTextName',
  'cfgTreeDecoratedNodeLabel', 'clearCfgTreeNodeTextNameCache']) {
  vm.runInContext(extract(name), context);
}
const settle = () => new Promise(resolve => setImmediate(resolve));
(async () => {
  // Rendering starts loading names without any click or branch selection.
  for (let slot = 0; slot < 5; ++slot) {
    const ref = 'v' + String(slot).padStart(2, '0');
    const branch = 'io/value/' + ref;
    assert.equal(context.cfgTreeDecoratedNodeLabel(branch, ref), ref);
    context.cfgTreeDecoratedNodeLabel(branch, 'Valeur dérivée ' + ref);
  }
  await settle();
  assert.equal(requests.length, 5); // Pending requests are deduplicated.
  assert.equal(renders, 5);
  for (let slot = 0; slot < 5; ++slot) {
    const ref = 'v' + String(slot).padStart(2, '0');
    assert.equal(context.cfgTreeDecoratedNodeLabel('io/value/' + ref, 'Valeur dérivée ' + ref),
      ref + ' [Débit piscine]');
  }
  assert.equal(requests[0], '/api/flowcfg/module?name=io%2Fvalue%2Fv00');
  assert.equal(requests.length, 5);
  // Existing name-cache invalidation after applying config also refreshes values.
  savedName = '  Température eau  ';
  context.clearCfgTreeNodeTextNameCache();
  context.cfgTreeDecoratedNodeLabel('io/value/v00', 'Valeur dérivée V00');
  await settle();
  assert.equal(context.cfgTreeDecoratedNodeLabel('io/value/v00', 'Valeur dérivée V00'), 'v00 [Température eau]');
  savedName = '';
  context.clearCfgTreeNodeTextNameCache();
  context.cfgTreeDecoratedNodeLabel('io/value/v00', 'Valeur dérivée V00');
  await settle();
  assert.equal(context.cfgTreeDecoratedNodeLabel('io/value/v00', 'Valeur dérivée V00'), 'v00');
  // Ordinary IO labels keep their original configuration keys and format.
  context.cfgTreeDecoratedNodeLabel('io/input/a00', 'Entrée analogique');
  await settle();
  assert.equal(context.cfgTreeDecoratedNodeLabel('io/input/a00', 'Entrée analogique'), 'a00 [Pression]');
  assert.equal(context.cfgTreeDecoratedNodeLabel('io/value', 'Valeurs dérivées'), 'Valeurs dérivées');
  assert.equal(context.cfgTreeNodeRefInfo('io/value/v00/name'), null);
  console.log('Derived-value tree: automatic names, stable labels, rename, empty names and existing IO verified.');
})().catch(error => { console.error(error); process.exitCode = 1; });

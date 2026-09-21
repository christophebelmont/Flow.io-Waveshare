const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../../data/webinterface/app.js'), 'utf8');
function extract(name) {
  const start = source.indexOf('    function ' + name + '(');
  assert(start >= 0, name);
  const end = source.indexOf('\n    function ', start + 1);
  return source.slice(start, end);
}
const context = vm.createContext({
  document: { createElement: () => ({}) },
  tr: (_key, fallback) => fallback,
});
for (const name of ['ioSummaryStateLabel', 'ioSummaryStateClass', 'createIoStateBadge', 'createIoDeviceStateBadge']) {
  vm.runInContext(extract(name), context);
}
function badge(interlock_state, overrides = {}, state = 'active') {
  return context.createIoDeviceStateBadge({ state, pool_device: {
    enabled: true, actual_on: false, block_code: 0, interlock_state, ...overrides,
  } });
}
assert.equal(badge(1).textContent, 'Arrêté');
assert.equal(badge(1).className, 'io-state-badge is-sleeping');
assert.match(badge(1).title, /dépendance/);
assert.equal(badge(2, { block_code: 2 }, 'error').textContent, 'Bloqué par interlock');
assert.equal(badge(3, { block_code: 2 }, 'error').textContent, 'Arrêt de sécurité : interlock');
assert.equal(badge(1, { block_code: 3 }, 'error').textContent, 'Erreur');
assert.equal(badge(3, { block_code: 3 }, 'error').textContent, 'Erreur');
assert.equal(badge(0, { actual_on: true }).textContent, 'Actif');
assert.equal(badge(1, { enabled: false }, 'manually_disabled').textContent, 'Désactivé manuellement');
assert.equal(context.createIoDeviceStateBadge({ state: 'error' }).textContent, 'Erreur');
console.log('I/O interlock presentation: OK');

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('data/webinterface/app.js', 'utf8');
const start = source.indexOf('    function buildConfigAction(');
const end = source.indexOf('    function renderConfigFields(', start);
class Element {
  constructor(tag) { this.tag = tag; this.children = []; this.handlers = {}; this.disabled = false; this.dataset = {}; }
  appendChild(child) { this.children.push(child); }
  setAttribute() {}
  addEventListener(event, handler) { this.handlers[event] = handler; }
}
const docs = JSON.parse(fs.readFileSync('src/Modules/IOModule/text/cfgdocs.fr.json')).docs;
const catalogs = ['fr', 'en'].map(lang => JSON.parse(fs.readFileSync(`src/Modules/IOModule/text/i18n.${lang}.json`)).translations);
let calls = 0, confirmed = false, resolveResponse;
const context = vm.createContext({
  document: { createElement: tag => new Element(tag) },
  window: { confirm: () => confirmed },
  cfgDocTr: key => key,
  closeAnchoredPopover() {},
  configDocFor: () => ({ ...docs['io/input/i15/counter_reset'], label: 'Reset counter' }),
  configEnumOptionsForField: () => null,
  normalizeDigitalInputConfigKey: (_module, key) => key,
  isDigitalInputConfigModule: () => true,
  parseDigitalInputModeValue: value => Number(value),
  URLSearchParams,
  fetch: (url, options) => {
    calls++;
    assert.equal(url, '/api/io/counter/reset');
    assert.equal(options.method, 'POST');
    assert.equal(options.body.get('id'), '79');
    return new Promise(resolve => { resolveResponse = resolve; });
  }
});
vm.runInContext(source.slice(start, end), context);
for (const name of ['isCounterModeOnlyConfigField', 'renderConfigFields']) {
  const begin = source.indexOf('    function ' + name + '(');
  const finish = source.indexOf('\n    function ', begin + 1);
  vm.runInContext(source.slice(begin, finish), context);
}
(async () => {
  for (let slot = 0; slot < 16; slot++) {
    const doc = docs[`io/input/i${String(slot).padStart(2, '0')}/counter_reset`];
    assert.equal(doc.widget, 'action');
    assert.equal(doc.counter_only, true);
    assert.equal(doc.action.params.id, 64 + slot);
    for (const catalog of catalogs) {
      for (const token of [doc.label_t, doc.help_t, doc.action.confirmation_t, doc.action.success_t]) assert(catalog[token]);
    }
  }
  for (const mode of [0, 1]) {
    const root = new Element('div');
    const values = Object.assign(Object.create({ mode }), { counter_reset: 0 });
    context.renderConfigFields(root, 'io/input/i15', values, { perFieldApply: true });
    assert.equal(root.children.length, 1);
    const row = root.children[0];
    assert.equal(row.hidden, mode === 0);
    const controls = row.children[1];
    assert.equal(controls.children.length, 1); // No ordinary field-apply button.
    assert.deepEqual(controls.children[0].children.map(child => child.tag), ['button', 'span']);
  }
  const doc = docs['io/input/i15/counter_reset'];
  const view = context.buildConfigAction({ ...doc, label: 'Reset counter' });
  const [button, status] = view.children;
  assert.equal(button.tag, 'button');
  assert.equal(calls, 0); // Rendering has no side effect.
  await button.handlers.click();
  assert.equal(calls, 0); // Cancellation has no side effect.
  confirmed = true;
  const pending = button.handlers.click();
  assert.equal(button.disabled, true);
  await button.handlers.click();
  assert.equal(calls, 1); // Ignore double click while awaiting persistence.
  assert.equal(status.textContent, 'config.action.pending');
  resolveResponse({ ok: true, json: async () => ({ ok: true }) });
  await pending;
  assert.equal(status.textContent, 'config.counter_reset.success');
  assert.equal(button.disabled, false);
  const failure = button.handlers.click();
  resolveResponse({ ok: false, json: async () => ({ ok: false }) });
  await failure;
  assert.equal(calls, 2); // No automatic retry on failure.
  assert.equal(status.textContent, 'config.action.failed');
  assert.equal(button.disabled, false);
  console.log('Counter reset UI: cancellation, double click, success, failure, translations and all slot IDs OK.');
})().catch(error => { console.error(error); process.exitCode = 1; });

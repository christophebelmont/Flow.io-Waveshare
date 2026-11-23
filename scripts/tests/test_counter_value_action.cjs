const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync('data/webinterface/app.js', 'utf8');
const start = source.indexOf('    function buildCounterValueField(');
const end = source.indexOf('    function renderConfigFields(', start);
assert(start >= 0 && end > start, 'manual counter editor must be defined');
assert(source.includes("if (key === 'counter_reset' && isDigitalInputConfigModule(moduleName))"));
assert(source.includes("doc: { counter_only: true }"), 'counter-only mode visibility must be kept');

class Element {
  constructor(tag) {
    this.tag = tag;
    this.children = [];
    this.handlers = {};
    this.disabled = false;
    this.value = '';
    this.isConnected = false;
    this.attributes = {};
    this.classes = {};
    this.classList = { toggle: (name, enabled) => { this.classes[name] = enabled; } };
  }
  appendChild(child) { this.children.push(child); }
  setAttribute(key, value) { this.attributes[key] = value; }
  addEventListener(name, handler) { this.handlers[name] = handler; }
}

let resolveGet;
let gets = 0;
let posts = 0;
let confirmChange = false;
let postFailure = false;
const postBodies = [];
const context = vm.createContext({
  document: { createElement: tag => new Element(tag) },
  window: { confirm: () => confirmChange },
  tr: (_key, fallback) => fallback,
  URLSearchParams,
  fetch: (url, options) => {
    if (url === '/api/io/counter/value?id=65') {
      assert.equal(options.cache, 'no-store');
      gets++;
      return new Promise(resolve => { resolveGet = resolve; });
    }
    assert.equal(url, '/api/io/counter/set');
    assert.equal(options.method, 'POST');
    assert.equal(options.cache, 'no-store');
    const params = options.body;
    assert.equal(params.get('id'), '65');
    postBodies.push(params.get('value'));
    posts++;
    return Promise.resolve({
      ok: !postFailure,
      json: async () => postFailure
        ? { ok: false, err: { code: 'CounterSetFailed' } }
        : { ok: true, value: params.get('value') }
    });
  }
});

vm.runInContext(source.slice(start, end), context);

async function flush() {
  await new Promise(resolve => setImmediate(resolve));
}

(async () => {
  const view = context.buildCounterValueField('io/input/i01');
  assert(view);
  view.row.isConnected = true;
  const controls = view.row.children[1].children;
  const [input, button, status] = controls;
  assert.equal(view.input, input);
  assert.equal(input.disabled, true);
  assert.equal(button.disabled, true);
  assert.equal(gets, 1);
  assert.equal(posts, 0);

  resolveGet({ ok: true, json: async () => ({ ok: true, value: '123' }) });
  await flush();
  assert.equal(input.disabled, false);
  assert.equal(input.value, '123');
  assert.equal(button.disabled, true, 'unchanged count cannot be resubmitted');

  input.value = '130';
  input.handlers.input();
  assert.equal(button.disabled, false);
  await button.handlers.click();
  assert.equal(posts, 0, 'cancelling confirmation must not write');

  confirmChange = true;
  await button.handlers.click();
  assert.equal(posts, 1);
  assert.deepEqual(postBodies, ['130']);
  assert.equal(input.value, '130');
  assert.equal(button.disabled, true);
  assert.equal(status.textContent, 'Valeur enregistrée et sauvegardée.');

  // Do not convert the value via JavaScript Number (2^53 precision loss).
  input.value = '9007199254740993';
  input.handlers.input();
  await button.handlers.click();
  assert.equal(posts, 2);
  assert.equal(postBodies[1], '9007199254740993');

  input.value = '18446744073709551616'; // uint64 maximum + 1
  input.handlers.input();
  assert.equal(button.disabled, true);

  input.value = '200';
  input.handlers.input();
  postFailure = true;
  await button.handlers.click();
  assert.equal(posts, 3);
  assert.equal(input.value, '200', 'failed writes must keep the unsaved draft');
  assert.equal(button.disabled, false, 'failure allows an explicit retry');
  assert.match(status.textContent, /Échec/);

  // Initial snapshot is never polled, even after multiple edits.
  assert.equal(gets, 1);
  assert.equal(context.buildCounterValueField('io/input/i99'), null);
  console.log('Counter value editor: one-shot load, mode visibility, cancel, save, retry, uint64 and no polling OK.');
})().catch(err => { console.error(err); process.exitCode = 1; });

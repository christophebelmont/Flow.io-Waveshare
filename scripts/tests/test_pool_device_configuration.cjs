const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../..');
const source = fs.readFileSync(path.join(root, 'data/webinterface/app.js'), 'utf8');
function extract(name) {
  let start = source.indexOf('    function ' + name + '(');
  if (start < 0) start = source.indexOf('    async function ' + name + '(');
  assert(start >= 0, name);
  const next = source.slice(start + 1).search(/\n    (?:async )?function /);
  const end = next < 0 ? -1 : start + 1 + next;
  assert(end > start, name);
  return source.slice(start, end);
}
class Element {
  constructor(tag) { this.tag = tag; this.children = []; this.dataset = {}; this.handlers = {}; }
  appendChild(child) { this.children.push(child); }
  setAttribute() {}
  addEventListener(event, fn) { this.handlers[event] = fn; }
  dispatchEvent() {}
}
const context = vm.createContext({
  document: { createElement: tag => new Element(tag) },
  Event: class {},
  tr: (_key, fallback) => fallback,
  nettoyerNomFlowCfg: value => value.trim(),
  storeConfigFieldInitialValue: () => {},
  isAnchoredPopoverOpenFor: () => false,
  openDependencyMaskPopover: (_trigger, popover) => { context.popover = popover; },
  flowCfgPoolDeviceOptions: Array.from({ length: 16 }, (_, slot) => ({
    value: slot, name: 'Device ' + slot, outputs: [slot], controllable: slot < 8
  }))
});
for (const name of ['poolLogicDeviceSlotRef', 'dependencyMaskSlotFromModule',
  'dependencyMaskOutputRef', 'dependencyMaskOptionLabel', 'dependencyMaskTriggerSummary',
  'updateDependencyMaskTrigger', 'buildDependencyMaskEditor']) {
  vm.runInContext(extract(name), context);
}
for (const currentSlot of [0, 8, 15]) {
  const editor = context.buildDependencyMaskEditor(null, 'depends_on_mask', 0, 'pdm/pd' + currentSlot);
  editor.element.children[0].handlers.click({ preventDefault() {}, stopPropagation() {} });
  const boxes = context.popover.children.map(row => row.children[0]);
  assert.deepEqual(boxes.map(box => Number(box.value)),
    Array.from({ length: 16 }, (_, i) => i).filter(i => i !== currentSlot));
  const target = currentSlot === 15 ? 14 : 15;
  const box = boxes.find(item => Number(item.value) === target);
  box.checked = true;
  box.handlers.change();
  assert.equal(Number(editor.input.value), 1 << target);
  box.checked = false;
  box.handlers.change();
  assert.equal(Number(editor.input.value), 0);
}
// Configuration metadata must hide the standalone PoolDevice branch from the tree.
const manifest = JSON.parse(fs.readFileSync(path.join(root,
  'src/Modules/PoolDeviceModule/text/cfgmods.fr.json'), 'utf8'));
assert.equal(manifest.docs.pdm.hidden, true);
for (const locale of ['fr', 'en']) {
  const catalog = JSON.parse(fs.readFileSync(path.join(root,
    `src/Modules/PoolDeviceModule/text/i18n.${locale}.json`), 'utf8'));
  assert.equal(catalog.translations[manifest.docs.pdm.label_t], 'PoolDevice');
}
console.log('PoolDevice configuration: hidden branch, all 15 valid dependencies, upper mask bits OK.');


// Configuration access must survive a driver change and a fresh browser session.
// The runtime output list is not a configuration-page association.
async function verifyDriverConfigurationAccess() {
  for (const name of ['flowCfgIoOutputSlotIndex', 'flowCfgPdmModuleForIoOutput',
    'loadFlowCfgPdmExtensionData']) vm.runInContext(extract(name), context);
  const calls = [];
  context.ensureFlowCfgPoolDeviceOptions = async () => {
    throw new Error('Configuration access must not depend on physical outputs');
  };
  context.fetchWithBusyRetry = async (url) => {
    calls.push(url);
    return { ok: true, json: async () => ({
      ok: true, data: { driver: JSON.stringify({ kind: 3, serial: { address: 7 } }) }
    }) };
  };
  for (let slot = 0; slot < 16; ++slot) {
    const extension = await context.loadFlowCfgPdmExtensionData(
      'io/output/d' + String(slot).padStart(2, '0'), {});
    assert.equal(extension.module, 'pdm/pd' + slot);
    assert.equal(JSON.parse(extension.data.driver).serial.address, 7);
    assert.equal(calls.at(-1), '/api/flowcfg/module?name=' + encodeURIComponent('pdm/pd' + slot));
  }
  const count = calls.length;
  assert.equal(await context.loadFlowCfgPdmExtensionData('io/input/a00', {}), null);
  assert.equal(calls.length, count);
  context.fetchWithBusyRetry = async () => ({ ok: false, json: async () => ({ ok: false }) });
  assert.equal(await context.loadFlowCfgPdmExtensionData('io/output/d00', {}), null);
  console.log('PoolDevice configuration: RS485 access survives restart for all 16 slots.');
}
verifyDriverConfigurationAccess().catch(error => { console.error(error); process.exitCode = 1; });

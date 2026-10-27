const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.join(__dirname, '../..');
const source = fs.readFileSync(path.join(root, 'data/webinterface/app.js'), 'utf8');
const docs = JSON.parse(fs.readFileSync(path.join(root, 'src/Modules/IOModule/text/cfgdocs.fr.json'))).docs;
for (const locale of ['fr', 'en']) {
  const translations = JSON.parse(fs.readFileSync(path.join(root, `data/webinterface/i18n/${locale}.json`))).translations;
  const context = vm.createContext({tr: (key, fallback) => translations[key] || fallback});
  for (const name of ['configIsBindingPortField', 'ioBindingResolutionLabel']) {
    const start = source.indexOf('    function ' + name + '(');
    const end = source.indexOf('\n    function ', start + 1);
    assert(start >= 0 && end > start);
    vm.runInContext(source.slice(start, end), context);
  }
  for (const slot of ['io/input/a00', 'io/input/i01', 'io/output/d00']) {
    assert(context.configIsBindingPortField(docs[slot + '/binding_port']));
    assert(context.configIsBindingPortField(docs[slot + '/secondary_binding_port']));
  }
  assert(!context.configIsBindingPortField({}));
  const row = {binding_reason: 'primary_absent', binding_restart_required: false};
  assert.equal(context.ioBindingResolutionLabel(row), translations['io.binding.primary_absent']);
  row.binding_restart_required = true;
  assert(context.ioBindingResolutionLabel(row).includes(translations['io.binding.restartRequired']));
}
console.log('Boot binding configuration and diagnostics: OK');

// Boolean enums must serialize their selected value, just like per-field saves.
const { loadFunctions } = require('./app_function_test_support.cjs');
const editor = vm.createContext({ nettoyerNomFlowCfg: (name) => name });
loadFunctions(source, editor, [
  'readConfigFieldValue', 'readConfigFieldValueStrict', 'buildPatchJsonFromFields'
]);
for (let slot = 0; slot < 16; ++slot) {
  const moduleName = `io/output/d${String(slot).padStart(2, '0')}`;
  for (const retain of [true, false]) {
    const select = {
      tagName: 'SELECT', value: String(retain),
      dataset: { key: 'retain_on_warm_reboot', kind: 'bool', module: moduleName }
    };
    const checkbox = {
      tagName: 'INPUT', checked: !retain,
      dataset: { key: 'initial_on', kind: 'bool', module: moduleName }
    };
    const fields = { querySelectorAll: () => [select, checkbox] };
    const patch = JSON.parse(editor.buildPatchJsonFromFields(fields, moduleName));
    assert.equal(patch[moduleName].retain_on_warm_reboot, retain);
    assert.equal(patch[moduleName].initial_on, !retain);
    assert.equal(patch[moduleName].retain_on_warm_reboot, editor.readConfigFieldValueStrict(select));
  }
}
console.log('Digital output boot behavior saves: OK');

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('data/webinterface/app.js', 'utf8');
const docs = JSON.parse(fs.readFileSync('src/Modules/IOModule/text/cfgdocs.fr.json')).docs;
const meta = JSON.parse(fs.readFileSync('src/Modules/IOModule/text/cfgmods.fr.json')).meta;
const options = meta.enum_sets.internal_analog_source;
assert.equal(options.length, 33);
assert.equal(new Set(options.map(option => option.value)).size, 33);
const context = vm.createContext({tr: (_key, fallback) => fallback});
for (const name of ['configConditionMatches', 'ioBindingResolutionLabel']) {
  const begin = source.indexOf('    function ' + name + '(');
  const end = source.indexOf('\n    function ', begin + 1);
  assert(begin >= 0 && end > begin);
  vm.runInContext(source.slice(begin, end), context);
}
for (let slot = 0; slot < 32; slot++) {
  const prefix = `io/input/a${String(slot).padStart(2, '0')}`;
  assert.equal(docs[prefix + '/source_value'].enum_set, 'internal_analog_source');
  for (const suffix of ['binding_port', 'secondary_binding_port', 'secondary_c0', 'secondary_c1']) {
    const condition = docs[prefix + '/' + suffix].visible_when;
    assert.equal(condition.field, 'source_value');
    assert(context.configConditionMatches(condition, 65535));
    assert(context.configConditionMatches(condition, '65535'));
    assert(!context.configConditionMatches(condition, 114));
    assert(!context.configConditionMatches(condition, 115));
  }
}
for (const locale of ['fr', 'en']) {
  const translations = JSON.parse(fs.readFileSync(`src/Modules/IOModule/text/i18n.${locale}.json`)).translations;
  for (const option of options) assert(translations[option.label_t]);
  assert(translations[docs['io/input/a00/source_value'].help_t]);
}
assert.match(context.ioBindingResolutionLabel({source_value: 115}), /115/);
assert.match(context.ioBindingResolutionLabel({source_value: 115, binding_restart_required: true}), /Redémarrage/);
assert.equal(context.ioBindingResolutionLabel({source_value: 65535, binding_reason: 'primary_present'}), 'primary_present');
console.log('Internal analog source configuration: typed options, translations, visibility and restart diagnostics OK');

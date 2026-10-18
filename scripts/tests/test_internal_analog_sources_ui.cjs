const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('data/webinterface/app.js', 'utf8');
const docs = JSON.parse(fs.readFileSync('src/Modules/IOModule/text/cfgdocs.fr.json')).docs;
const meta = JSON.parse(fs.readFileSync('src/Modules/IOModule/text/cfgmods.fr.json')).meta;
assert.equal(meta.enum_sets.internal_analog_source, undefined);
for (let slot = 0; slot < 32; slot++) {
  const prefix = `io/input/a${String(slot).padStart(2, '0')}`;
  assert.equal(docs[prefix + '/source_value'], undefined);
  for (const suffix of ['binding_port', 'secondary_binding_port', 'secondary_c0', 'secondary_c1']) {
    assert(docs[prefix + '/' + suffix]);
    assert.equal(docs[prefix + '/' + suffix].visible_when, undefined);
  }
}
const moduleSource = fs.readFileSync('src/Modules/IOModule/IOModule.cpp', 'utf8');
assert(!moduleSource.includes('cfg.registerVar(vars.sourceValueVar'));
assert(moduleSource.includes('analogCfg_[i].sourceValue = VALUE_INVALID;'));
const context = vm.createContext({tr: (_key, fallback) => fallback});
const begin = source.indexOf('    function ioBindingResolutionLabel(');
const end = source.indexOf('    function ', begin + 14);
vm.runInContext(source.slice(begin, end), context);
assert.equal(context.ioBindingResolutionLabel({binding_reason:'primary_present'}), 'primary_present');
assert.match(context.ioBindingResolutionLabel({binding_reason:'secondary_present',binding_restart_required:true}), /Redémarrage/);
console.log('Analog inputs: fixed physical source, no persisted selector, both bindings and calibration visible OK');

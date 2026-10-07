const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { loadFunctions } = require('./app_function_test_support.cjs');
const source = fs.readFileSync('data/webinterface/app.js', 'utf8');
const context = vm.createContext({ tr: (_key, fallback) => fallback });
vm.runInContext(source.slice(source.indexOf('    const derivedValueSlotCount'), source.indexOf('    function derivedValueModuleName')), context);
loadFunctions(source, context, ['derivedValueModuleName', 'derivedValueNormalizeNumber',
    'derivedValueEntryFromData', 'derivedValueSnapshot', 'derivedValueBuildPatch']);
const data = { enabled: true, name: 'Volume', unit: 'L', precision: 6, expr: 'i01.count*k3', aggregation: 2,
    k0: 1, k1: 0, k2: 0, k3: 0.5 };
const entry = context.derivedValueEntryFromData(0, data);
entry.original = context.derivedValueSnapshot(entry);
assert.equal(entry.k3, 0.5);
assert.equal(Object.keys(context.derivedValueBuildPatch([entry])).length, 0);
entry.k3 = '0.25';
assert.equal(context.derivedValueBuildPatch([entry])['io/value/v00'].k3, 0.25);
for (const text of ['0,5', 'abc', '1e309', '', ' ', '0x10']) {
    entry.k3 = text;
    assert.throws(() => context.derivedValueBuildPatch([entry]));
    assert.equal(entry.k3, text); // Invalid editing text is preserved, never changed to zero.
}
entry.k3 = '1.25e-2';
assert.equal(context.derivedValueBuildPatch([entry])['io/value/v00'].k3, 0.0125);
for (const precision of [NaN, 1.5, -1, 7]) {
    entry.precision = precision;
    assert.throws(() => context.derivedValueBuildPatch([entry]));
}
entry.precision = 6;
entry.expr = ' ';
assert.throws(() => context.derivedValueBuildPatch([entry]));
console.log('Derived editor: K3 round trip, exact patch and rejected invalid coefficients OK');

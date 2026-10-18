const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('data/webinterface/app.js', 'utf8');
const start = source.indexOf('    function dataExposurePatch(');
const end = source.indexOf('    async function loadDataValuesPage', start);
const context = vm.createContext({});
vm.runInContext(source.slice(start, end), context);
const original = {path:'data/value/d117',ha:false,originalHa:false,history:true,originalHistory:true,usable:true,originalUsable:true};
assert.equal(JSON.stringify(context.dataExposurePatch([original])), '{}');
assert.equal(JSON.stringify(context.dataExposurePatch([{...original,ha:true}])), '{"data/value/d117":{"publish_ha":true}}');
assert.equal(JSON.stringify(context.dataExposurePatch([{...original,history:false}])), '{"data/value/d117":{"history":false}}');
for (const locale of ['fr','en']) {
 const translations = JSON.parse(fs.readFileSync(`data/webinterface/i18n/${locale}.json`)).translations;
 for (const key of ['title','intro','restart','history','saved','restart']) assert(translations['data.'+key]);
}
console.log('Data exposure: independent policy patches, unchanged values omitted, bilingual labels OK');

assert.equal(JSON.stringify(context.dataExposurePatch([{...original,usable:false}])), '{"data/value/d117":{"usable":false}}');
const usableStart = source.indexOf('    function dataReferenceUsable(');
const usableEnd = source.indexOf('    const derivedEntityNames', usableStart);
vm.runInContext('let dataValueCatalog = [{reference:"pd2.tank.fill_percent", usable:false}];' + source.slice(usableStart, usableEnd), context);
assert.equal(context.dataReferenceUsable('pd2.tank.fill_percent'), false);
assert.equal(context.dataReferenceUsable('a00'), false);

const optionsStart = source.indexOf('    function dynamicTftRuntimeUiOptions(');
const optionsEnd = source.indexOf('    // ---------- Derived values page', optionsStart);
vm.runInContext(`
  dataValueCatalog = [
    {id:117, reference:'pd2.tank.fill_percent', usable:false, runtime_ids:[32885]},
    {id:96, reference:'v00', usable:false, available:true, runtime_ids:[32864]},
    {id:144, reference:'pd0.uptime_day_s', usable:true, available:false, runtime_ids:[32912]},
    {id:116, reference:'pd2.tank.remaining_l', usable:true, available:true, runtime_ids:[32884],name:'Chlore',unit:'L'}
  ];
  function derivedSlotFromEnumOption() { return -1; }
` + source.slice(optionsStart, optionsEnd), context);
const visible = context.dynamicTftRuntimeUiOptions([
  {value:32885,data_value_id:117},
  {value:32912,data_value_id:144}, {value:32913,data_value_id:145},
  {value:32864,data_value_id:96}, {value:32884,data_value_id:116}, {value:2101}
]);
assert.deepEqual(Array.from(visible, option => option.value), [32884,2101]);
console.log('Usage capability: exact patch, formula tokens and catalog display selectors OK');

assert.equal(context.dataReferenceUsable('pd2.tank.remaining_l'), true);
assert.equal(context.dataReferenceUsable('pd0.uptime_day_s'), false);
console.log('Unavailable or unknown data excluded even with interface enabled; recovery remains selectable');

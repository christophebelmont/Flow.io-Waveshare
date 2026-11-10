'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync('data/webinterface/app.js', 'utf8');
const bandsStart = source.indexOf('    function createFlowFiveZoneBands(');
const bandsEnd = source.indexOf('    function resolveFlowGaugeValueColor(', bandsStart);
const rangeStart = source.indexOf('    function poolSondeRangeFromEntry(');
const rangeEnd = source.indexOf('    function enrichPoolSondeSlotsWithRanges(', rangeStart);
const dashboardStart = source.indexOf('      const poolMetricRows = [');
const dashboardEnd = source.indexOf('      const poolStateGrid =', dashboardStart);
assert(bandsStart >= 0 && bandsEnd > bandsStart);
assert(rangeStart >= 0 && rangeEnd > rangeStart);
assert(dashboardStart >= 0 && dashboardEnd > dashboardStart);

const dashboardRows = source.slice(dashboardStart, dashboardEnd);
assert(!/bands:\s*createFlowFiveZoneBands\(\{\s*min:/.test(dashboardRows),
  'dashboard threshold bands must not retain old scale endpoints');
assert(!/bands:\s*createFlowFiveZoneBands\([\s\S]*?max:\s*(?:40|45|8\.4|900)/.test(dashboardRows),
  'dashboard band scales must come from the registry range');

const context = vm.createContext({});
vm.runInContext(`let dataValueCatalog = [
  {id:4,has_display_range:true,display_min:12,display_max:31}
];\n` + source.slice(bandsStart, bandsEnd) + source.slice(rangeStart, rangeEnd), context);
const bands = context.createFlowFiveZoneBands({
  criticalLowEnd: 8, warningLowEnd: 14, warningHighStart: 30, criticalHighStart: 34
});
assert.equal(bands[0].from, -Number.MAX_VALUE);
assert.equal(bands.at(-1).to, Number.MAX_VALUE);
const effectiveRange = context.poolSondeRangeFromEntry({dataValueId:4});
assert.equal(effectiveRange.min, 12);
assert.equal(effectiveRange.max, 31);
console.log('Dashboard display ranges: effective registry range is used and band endpoints no longer override it');

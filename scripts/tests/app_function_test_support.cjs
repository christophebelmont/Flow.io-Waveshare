const assert = require('node:assert/strict');
const vm = require('node:vm');
function loadFunctions(source, context, names) {
  for (const name of names) {
    const start = source.search(new RegExp('^    (?:async )?function ' + name + '\\(', 'm'));
    assert(start >= 0, name);
    const next = source.slice(start + 1).search(/^    (?:async )?function /m);
    assert(next >= 0, name);
    vm.runInContext(source.slice(start, start + 1 + next), context);
  }
}
module.exports = { loadFunctions };

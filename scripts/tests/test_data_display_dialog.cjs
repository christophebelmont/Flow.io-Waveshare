'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');

const source = fs.readFileSync('data/webinterface/app.js', 'utf8');
const start = source.indexOf('    function openDataDisplayDialog(');
const end = source.indexOf('    function buildDataValuesCard(', start);
assert(start >= 0 && end > start, 'display range dialog implementation exists');
const dialog = source.slice(start, end);
assert.match(dialog, /id="dataDisplayCancel"[^>]*type="button"/,
  'cancel button must not submit the form or trigger required-field validation');
assert.match(dialog, /#dataDisplayCancel'\)\.addEventListener\('click',\s*\(\)\s*=>\s*dialog\.close\('cancel'\)\)/,
  'cancel button closes the dialog without saving');
assert.match(dialog, /id="dataDisplaySave" class="btn-primary" type="button"/,
  'saving remains an explicit action');
console.log('Display range dialog: cancel closes without validation or save');

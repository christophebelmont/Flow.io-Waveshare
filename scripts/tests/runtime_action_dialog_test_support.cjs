const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const project = path.resolve(__dirname, '../..');

function loadDialog(moduleFolder, valueId, runtimeId) {
  const base = path.join(project, 'src/Modules', moduleFolder, 'text');
  const catalog = JSON.parse(fs.readFileSync(path.join(base, 'i18n.fr.json'), 'utf8')).translations;
  const entry = JSON.parse(fs.readFileSync(path.join(base, 'runtimeui.json'), 'utf8')).values.find(value => value.valueId === valueId);
  function resolve(value) {
    if (Array.isArray(value)) return value.map(resolve);
    if (!value || typeof value !== 'object') return value;
    return Object.fromEntries(Object.entries(value).map(([key, item]) =>
      key.endsWith('_t') ? [key.slice(0, -2), catalog[item]] : [key, resolve(item)]));
  }
  entry.id = runtimeId;
  entry.displayConfig.actionDialog = resolve(entry.displayConfig.actionDialog);
  return entry;
}

async function installDialog(page, entry) {
  const app = fs.readFileSync(path.join(project, 'data/webinterface/app.js'), 'utf8');
  const start = app.indexOf('    function runtimeActionKey(');
  const end = app.indexOf('    function buildDashboardDualStateTile(', start);
  assert(start >= 0 && end > start, 'Runtime action functions must be present');
  await page.evaluate(({ runtimeActions, entry }) => {
    window.testEntry = entry;
    window.tr = (_key, fallback) => fallback;
    window.runtimeMeasureDisplayConfig = entry => entry.displayConfig || {};
    window.normalizeRuntimeMeasureDomainKey = value => value;
    window.refreshPoolMeasuresView = () => {};
    window.invalidatePoolDashboardSlots = () => { testState.invalidations++; };
    window.loadPoolMeasureDomain = async domain => { testState.refreshes.push(domain); };
    window.refreshPoolOverview = async () => { testState.refreshes.push('overview'); };
    window.extractApiErrorMessage = data => data.error;
    const script = document.createElement('script');
    script.textContent = 'let runtimeActionBusyKey = ""; let runtimeActionDialog = null; let runtimeActionDialogRefresh = null; const runtimeActionFeedback = new Map();\n'
      + runtimeActions + '\ndocument.querySelector(".status-card").appendChild(buildRuntimeActionDialogButton(testEntry));';
    document.body.appendChild(script);
  }, { runtimeActions: app.slice(start, end), entry });
}

module.exports = { project, loadDialog, installDialog };

'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

const project = path.resolve(__dirname, '../..');
const app = fs.readFileSync(path.join(project, 'data/webinterface/app.js'), 'utf8');

function sourceBetween(startText, endText) {
  const start = app.indexOf(startText);
  const end = app.indexOf(endText, start);
  assert(start >= 0 && end > start, `Missing source section: ${startText}`);
  return app.slice(start, end);
}

async function main() {
  const popupSwitchSource = sourceBetween('      function buildSwitch(target, column) {', '      function refreshTable(');
  const dashboardSwitchSource = sourceBetween('    function buildDashboardDualStateTile(', '    function reconcileDashboardMeasureCards(');
  const configSwitchSource = sourceBetween("        } else if (typeof value === 'boolean') {", "        } else if (configNumericKind(doc, value) !== 'string') {");
  assert(popupSwitchSource.includes('buildFlowSwitch('));
  assert(dashboardSwitchSource.includes('buildFlowSwitch('));
  assert(configSwitchSource.includes('buildFlowSwitch('));

  const sharedSource = sourceBetween('    function setRuntimeActionText(', '    function buildRuntimeActionCell(');
  const browser = await chromium.launch({
    headless: true,
    ...(process.env.FLOWIO_TEST_BROWSER ? { executablePath: process.env.FLOWIO_TEST_BROWSER } : {})
  });
  try {
    const page = await browser.newPage({ viewport: { width: 900, height: 520 } });
    await page.setContent('<html lang="fr"><body><main id="test-root"></main></body></html>');
    await page.addStyleTag({ content: fs.readFileSync(path.join(project, 'data/webinterface/app-core.css'), 'utf8') });
    await page.evaluate(({ sharedSource, dashboardSwitchSource }) => {
      const script = document.createElement('script');
      script.textContent = `
        const dashboardDualStateTileViews = new WeakMap();
        function tr(_key, fallback) { return fallback; }
        ${sharedSource}
        ${dashboardSwitchSource}
        const root = document.getElementById('test-root');
        const dashboardOptions = {
          action: { id: 'set-mode' },
          activeText: 'Actif',
          inactiveText: 'Inactif',
          onAction: requested => window.requestedStates.push(requested)
        };
        window.requestedStates = [];
        window.dashboardTile = buildDashboardDualStateTile('Mode automatique', false, dashboardOptions);
        root.appendChild(window.dashboardTile);
        window.updateDashboard = (value, pending = false) => {
          dashboardDualStateTileViews.get(window.dashboardTile).update(
            'Mode automatique', value, { ...dashboardOptions, pending }
          );
        };
        window.configSwitch = buildFlowSwitch({ checked: false, label: 'Protection antigel' });
        root.appendChild(window.configSwitch.element);
      `;
      document.body.appendChild(script);
    }, { sharedSource, dashboardSwitchSource });

    const dashboard = page.getByRole('switch', { name: 'Mode automatique : Inactif' });
    const config = page.getByRole('switch', { name: 'Protection antigel' });
    assert.equal(await dashboard.isChecked(), false);
    await dashboard.click();
    assert.equal(await dashboard.isChecked(), false, 'Remote commands retain the confirmed state');
    assert.deepEqual(await page.evaluate(() => window.requestedStates), [true]);

    await page.evaluate(() => window.updateDashboard(true));
    assert.equal(await page.getByRole('switch', { name: 'Mode automatique : Actif' }).isChecked(), true);
    await config.check();
    assert.equal(await config.isChecked(), true, 'Configuration edits update immediately');
    await page.waitForTimeout(250);

    const metrics = await page.locator('.flow-switch-visual').evaluateAll(elements => elements.map(element => {
      const track = element.querySelector('.flow-switch-track');
      const thumb = element.querySelector('.flow-switch-thumb');
      return {
        track: [track.offsetWidth, track.offsetHeight],
        thumb: [thumb.offsetWidth, thumb.offsetHeight],
        thumbColor: getComputedStyle(thumb).backgroundColor,
        transition: getComputedStyle(thumb).transitionProperty,
        transform: getComputedStyle(thumb).transform
      };
    }));
    assert(metrics.every(metric => metric.track[0] === 36 && metric.track[1] === 14));
    assert(metrics.every(metric => metric.thumb[0] === 20 && metric.thumb[1] === 20));
    assert(metrics.every(metric => metric.transition.includes('transform')));
    assert(metrics.every(metric => metric.transform.endsWith(', 16, 0)')));
    assert.equal(metrics[0].thumbColor, metrics[1].thumbColor, 'Dashboard and Configuration share the active green');

    await page.evaluate(() => window.updateDashboard(true, true));
    const pendingDashboard = page.getByRole('switch', { name: 'Mode automatique : Application…' });
    assert.equal(await pendingDashboard.isDisabled(), true);
    assert.equal(await pendingDashboard.getAttribute('aria-busy'), 'true');
    console.log('Flow switch consolidation: shared green geometry, animation and context behavior passed');
  } finally {
    await browser.close();
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});

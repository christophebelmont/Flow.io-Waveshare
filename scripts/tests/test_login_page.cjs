'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

const project = path.resolve(__dirname, '../..');
const source = fs.readFileSync(
  path.join(project, 'src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp'),
  'utf8'
);
const pageMatch = source.match(/static const char kLoginPageHtml\[\] PROGMEM = R"HTML\(([\s\S]*?)\)HTML";/);
assert(pageMatch, 'The embedded login page is present');
const html = pageMatch[1].replace('<head>', '<head><base href="http://flowio.local/">');

async function assertInsideViewport(locator, width, height) {
  const box = await locator.boundingBox();
  assert(box, 'The expected element is visible');
  assert(box.x >= 0 && box.y >= 0, 'The element starts inside the viewport');
  assert(box.x + box.width <= width, 'The element fits the viewport width');
  assert(box.y + box.height <= height, 'The element fits the viewport height');
}

async function main() {
  const browser = await chromium.launch({
    headless: true,
    ...(process.env.FLOWIO_TEST_BROWSER ? { executablePath: process.env.FLOWIO_TEST_BROWSER } : {})
  });

  try {
    const page = await browser.newPage({ viewport: { width: 1440, height: 960 } });
    const browserErrors = [];
    page.on('pageerror', error => browserErrors.push(error.message));
    await page.route('http://flowio.local/webinterface/logo-flowio.png', route => route.fulfill({
      contentType: 'image/png',
      body: fs.readFileSync(path.join(project, 'data/webinterface/logo-flowio.png'))
    }));
    await page.route('http://flowio.local/webinterface/favicon.png', route => route.fulfill({ status: 204 }));
    await page.route('http://flowio.local/api/auth/login', async route => {
      const parameters = new URLSearchParams(route.request().postData() || '');
      assert.equal(parameters.get('username'), 'admin');
      assert.equal(parameters.get('password'), 'secret');
      await route.fulfill({ contentType: 'application/json', body: '{"ok":false}' });
    });

    await page.setContent(html, { waitUntil: 'load' });
    assert.equal(await page.locator('.tagline').textContent(), 'The Open Platform for the Connected Pool');
    assert.equal(await page.locator('.intro-brand').evaluate(element => getComputedStyle(element).display), 'block');
    assert.equal(await page.locator('.card .brand').evaluate(element => getComputedStyle(element).display), 'none');
    await assertInsideViewport(page.locator('.card'), 1440, 960);
    const desktopColumns = await page.locator('.shell').evaluate(element => getComputedStyle(element).gridTemplateColumns);
    assert(desktopColumns.split(' ').length >= 2, 'Desktop uses the presentation and login columns');
    await page.screenshot({ animations: 'disabled', path: '/tmp/flowio-login-desktop.png' });

    const password = page.locator('#pass');
    await password.fill('secret');
    await page.locator('#togglePass').click();
    assert.equal(await password.getAttribute('type'), 'text');
    assert.equal(await page.locator('#togglePass').getAttribute('aria-pressed'), 'true');
    await page.locator('#user').fill('admin');
    await page.locator('#submit').click();
    await page.getByText('Connexion refusée', { exact: true }).waitFor();
    await page.locator('#togglePass').click();
    await page.locator('#loginForm').evaluate(form => form.reset());
    await page.locator('#status').evaluate(element => { element.textContent = ''; });

    await page.setViewportSize({ width: 390, height: 844 });
    await assertInsideViewport(page.locator('.card'), 390, 844);
    assert.equal(await page.locator('.intro').evaluate(element => getComputedStyle(element).display), 'none');
    assert.equal(await page.locator('.card .brand').evaluate(element => getComputedStyle(element).display), 'flex');
    assert(await page.locator('body').evaluate(element => element.scrollWidth <= 390), 'Mobile has no horizontal overflow');
    await page.screenshot({ animations: 'disabled', path: '/tmp/flowio-login-mobile.png' });

    assert.deepEqual(browserErrors, []);
    console.log('Login page: desktop, mobile, password visibility and submit states passed');
  } finally {
    await browser.close();
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});

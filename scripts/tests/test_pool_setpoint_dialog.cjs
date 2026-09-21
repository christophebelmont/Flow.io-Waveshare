'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { chromium } = require('playwright');
const { project, loadDialog, installDialog } = require('./runtime_action_dialog_test_support.cjs');
const entry = loadDialog('PoolDeviceModule', 5, 2305);
const commands = [];
const devices = [
  { value:0, kind:0, name:'Relais', label:'Relais', deviceId:'pd0' },
  { value:1, kind:1, name:'Pompe paliers', label:'Pompe paliers', deviceId:'pd1', steps:[30,60,100], setpoint:60 },
  { value:2, kind:3, name:'Pompe RS485', label:'Pompe RS485', deviceId:'pd2', minimum:0, maximum:100, setpoint:50, observedSetpoint:50 }
].map(x=>({...x, actualOn:false, desiredOn:false, controllable:true, quality:1,
  running:{day_s:0,week_s:0,month_s:0,total_s:0},injected:{day_ml:0,week_ml:0,month_ml:0,total_ml:0}}));
const server = http.createServer((req,res)=>{
  res.setHeader('Content-Type','application/json');
  if (req.url === entry.displayConfig.actionDialog.optionsUrl) res.end(JSON.stringify({options:devices}));
  else if (req.url === '/api/runtime/action') {
    let body='';req.on('data',chunk=>body+=chunk);req.on('end',()=>{
      const command=Object.fromEntries(new URLSearchParams(body));commands.push(command);
      const device=devices.find(x=>x.value===Number(command.target));
      if(command.action_id==='setpoint') device.setpoint=Number(command.input);
      res.end('{"ok":true}');
    });
  } else { res.setHeader('Content-Type','text/html');res.end('<html><body><div class="status-card"><h3>Équipements</h3></div></body></html>'); }
});
(async()=>{
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const browser=await chromium.launch({headless:true,...(process.env.FLOWIO_TEST_BROWSER?{executablePath:process.env.FLOWIO_TEST_BROWSER}:{})});
  try {
    const page=await browser.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));
    await page.goto(`http://127.0.0.1:${server.address().port}`);
    await page.evaluate(()=>{
      window.testState={invalidations:0,refreshes:[]};
      window.fetchWithBusyRetry=(url,options)=>fetch(url,options);
      window.fetchOkJson=async url=>(await fetch(url)).json();
    });
    await installDialog(page,entry);
    await page.locator('.status-card button').click();
    await page.waitForSelector('.runtime-setpoint select');
    const steps=page.locator('.runtime-setpoint select');
    assert.deepEqual(await steps.locator('option').allTextContents(),['30','60','100']);
    await steps.selectOption('100');
    await page.waitForFunction(()=>!document.querySelector('.runtime-setpoint select').disabled);
    assert.equal(commands[0].action_id,'setpoint');assert.equal(commands[0].target,'1');assert.equal(commands[0].input,'100');
    const continuous=page.locator('.runtime-setpoint input');
    await continuous.fill('65.5');await continuous.dispatchEvent('change');
    await page.waitForFunction(()=>!document.querySelector('.runtime-setpoint input').disabled);
    assert.equal(commands[1].target,'2');assert.equal(commands[1].input,'65.5');
    // The configuration form must commit a complete serial object on mode change.
    const app=fs.readFileSync(path.join(project,'data/webinterface/app.js'),'utf8');
    const editor=app.slice(app.indexOf('    function buildPoolDriverEditor('),app.indexOf('    function renderConfigFields('));
    const config=await page.evaluate(code=>{
      const script=document.createElement('script');script.textContent=code;document.body.appendChild(script);
      const view=buildPoolDriverEditor('{"kind":0,"outputs":[0]}');document.body.appendChild(view.element);
      const kind=view.element.querySelector('select');kind.value='3';kind.dispatchEvent(new Event('change'));
      return JSON.parse(view.input.value);
    },editor);
    assert.equal(config.kind,3);assert.equal(config.serial.run.function,6);assert.equal(config.serial.setpoint.address,1);
    assert.deepEqual(errors,[]);
    console.log('Pool setpoint dialog: discrete and continuous commands, state, configuration editor passed');
  } finally { await browser.close(); await new Promise(resolve=>server.close(resolve)); }
})().catch(err=>{console.error(err);server.close();process.exitCode=1;});

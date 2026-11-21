#!/usr/bin/env node
/* Summarize one Flow.io serial OTA log. No dependencies or device access.
 * Run: node scripts/tests/summarize_ota_trace.mjs ota_serial.log
 */
function summarizeOtaLog(log) {
  const out={lines:0,lastStage:null,lastOperation:null,progressBytes:null,minInternalRam:null,minLargestBlock:null,minStackFree:null,maxFlashWriteUs:0,maxModbusTickUs:0,maxModbusClients:0,lastModbusEnabled:null,resetReason:null};
  const pick=(line,key)=>{const m=line.match(new RegExp("(?:^|\\s)"+key+"=([^\\s]+)"));return m?m[1]:null;};
  const min=(a,b)=>b===null||!Number.isFinite(Number(b))?a:(a===null?Number(b):Math.min(a,Number(b)));
  for(const line of log.split(/\r?\n/)){
    if(!line.trim())continue;out.lines++;
    if(line.includes("[OTA-TRACE]")){
      const stage=pick(line,"stage"),op=pick(line,"op"),bytes=pick(line,"received");
      if(stage)out.lastStage=stage;
      if(op)out.lastOperation=op;
      if(bytes)out.progressBytes=bytes;
      out.maxFlashWriteUs=Math.max(out.maxFlashWriteUs,Number(pick(line,"flash_max_us")||0));
    }
    if(line.includes("[OTA-MEM]")){
      out.minInternalRam=min(out.minInternalRam,pick(line,"internal"));
      out.minLargestBlock=min(out.minLargestBlock,pick(line,"largest"));
      out.minStackFree=min(out.minStackFree,pick(line,"stack_free"));
    }
    if(line.includes("[OTA-NET]")){
      out.lastModbusEnabled=pick(line,"enabled")||out.lastModbusEnabled;
      out.maxModbusTickUs=Math.max(out.maxModbusTickUs,Number(pick(line,"tick_peak_us")||0));
      const clients=pick(line,"clients");
      if(clients)out.maxModbusClients=Math.max(out.maxModbusClients,clients.split(",").reduce((a,b)=>a+Number(b),0));
    }
    const reset=line.match(/"reset_reason"\s*:\s*(\d+)/);
    if(reset)out.resetReason=Number(reset[1]);
  }
  return out;
}
import fs from "node:fs";
const input=process.argv[2];
if(!input){console.error("Usage: node scripts/tests/summarize_ota_trace.mjs <serial.log>");process.exit(2);}
console.log(JSON.stringify(summarizeOtaLog(fs.readFileSync(input,"utf8")),null,2));

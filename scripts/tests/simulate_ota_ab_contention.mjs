#!/usr/bin/env node
/* Host-only, deterministic A/B OTA transaction + network-contention model.
 * NOT a test of Flow.io's actual ESP32, lwIP, FreeRTOS, RAM or flash timings.
 * Virtual HTTP chunk delays increase with 0/1/2 Modbus TCP listening ports.
 * Fault injection tests safety invariants (never commit incomplete images).
 * Run: node scripts/tests/simulate_ota_ab_contention.mjs
 */
function simulateOta(ports, seed, fault) {
  const sizes=[1572864,2816944], timeoutMs=15000;
  let state=seed>>>0, verified=0, requests=0, bytes=[0,0];
  const rnd=()=>{state=(Math.imul(state,1664525)+1013904223)>>>0;return state;};
  const fail=(reason,outcome="failed",slot="app1")=>({outcome,slot,verified,requests,bytes,reason});
  if(fault==="low_memory") return fail("allocation refused");
  for(let image=0;image<2;image++){
    requests++;
    while(bytes[image]<sizes[image]){
      // Virtual-time contention only: bounded jitter for 0..2 Modbus listeners.
      let waitMs=1+(rnd()%(12+ports*30));
      if(image===0 && bytes[0]>=262144 && bytes[0]<263168) {
        if(fault==="stall_long") waitMs=15001;
        if(fault==="stall_short") waitMs=14999;
      }
      if(waitMs>timeoutMs)return fail("HTTP stream idle timeout");
      let chunk=Math.min(1024,sizes[image]-bytes[image]);
      if(image===0 && fault==="cut_271215"){
        if(bytes[0]>=271215)return fail("incomplete SPIFFS");
        chunk=Math.min(chunk,271215-bytes[0]);
      }
      bytes[image]+=chunk;
      if(image===0 && fault==="reset_during_download" && bytes[0]>=262144)
        return fail("power loss during SPIFFS","interrupted");
    }
    if(image===0 && fault==="bad_sha")return fail("SPIFFS sha256 mismatch");
    verified++;
    if(image===0 && fault==="reset_between_images")return fail("power loss after SPIFFS verify","interrupted");
  }
  if(fault==="reset_before_commit")return fail("power loss before commit","interrupted");
  // Boot may change only after both images were verified and receipt checkpointed.
  const committed={outcome:"succeeded",slot:"app0",verified,requests,bytes,reason:"paired commit"};
  if(fault==="reset_after_commit")committed.reason="reboot pending -> success on boot";
  return committed;
}
function runSuite(){
  const sizes=[1572864,2816944], faults=["none","stall_short","stall_long","cut_271215","bad_sha","low_memory","reset_during_download","reset_between_images","reset_before_commit","reset_after_commit"];
  const expected={none:"succeeded",stall_short:"succeeded",stall_long:"failed",cut_271215:"failed",bad_sha:"failed",low_memory:"failed",reset_during_download:"interrupted",reset_between_images:"interrupted",reset_before_commit:"interrupted",reset_after_commit:"succeeded"};
  const report={scenarios:0,ports:{"0":0,"1":0,"2":0},outcomes:{succeeded:0,failed:0,interrupted:0},faults:{}};
  const assert=(check,msg)=>{if(!check)throw Error(msg);};
  for(let ports=0;ports<=2;ports++){
    for(const fault of faults){
      let count=0;
      for(let seed=1;seed<=100;seed++){
        const r=simulateOta(ports,seed,fault);
        assert(r.outcome===expected[fault],`unexpected result ports=${ports} fault=${fault} seed=${seed} got=${r.outcome}`);
        assert(r.bytes.every((n,i)=>n<=sizes[i]),"image byte overflow");
        if(r.outcome==="succeeded"){
          assert(r.verified===2 && r.requests===2 && r.bytes.every((n,i)=>n===sizes[i])&&r.slot==="app0","unsafe paired commit");
        }else{
          assert(r.slot==="app1" && r.verified<2 || (r.slot==="app1" && fault==="reset_before_commit" && r.verified===2),"invalid failed transaction slot");
        }
        if(fault==="cut_271215")assert(r.bytes[0]===271215&&r.requests===1,"cut must not start firmware download");
        report.scenarios++;report.ports[String(ports)]++;report.outcomes[r.outcome]++;count++;
      }
      report.faults[fault]=(report.faults[fault]||0)+count;
    }
  }
  return report;
}
console.log(JSON.stringify(runSuite(),null,2));

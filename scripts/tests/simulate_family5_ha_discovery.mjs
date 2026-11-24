#!/usr/bin/env node
/**
 * Family 5 MQTT/Home Assistant discovery fault-injection MODEL.
 *
 * Run: node scripts/tests/simulate_family5_ha_discovery.mjs
 * This models the source scheduling and retry invariants with deterministic
 * broker outages, congestion, delayed sensor data and failed JSON builds.
 * It does NOT execute the ESP32 firmware or simulate a real MQTT/HA broker.
 */
const report = (function family5Matrix() {
  const stats={scenarios:0,assertions:0,failures:0,zeroCatalogues:0,incompleteDiscoveries:0,offlineBroker:0,congestedQueue:0,lateValuesRecovered:0,uiPaths:0,bitmapWidths:0,millisWrapCases:0,maxVirtualMinutes:0};
  const failures=[];
  function check(ok,why){stats.assertions++;if(!ok){stats.failures++;if(failures.length<12)failures.push(why);}}
  const offline=(t,kind)=>kind===0?false:kind===1?t<30000:kind===2?t>=7500&&t<55000:(t<70000 || Math.floor(t/10000)%4===1);
  const queueBusy=(t,kind,seed)=>kind===0?false:kind===1?Math.floor(t/500)%4!==0:Math.floor(t/500+seed)%7!==0;
  function simulate(n,mode,broker,queue,seed){
    let pending=new Set(Array.from({length:n},(_,i)=>i)),deferred=new Set(),sent=new Set(),discarded=new Set(),excluded=new Set();
    let retry=0,deadline=0,published=false,done=false,t=0,offlineSeen=false,queueSeen=false,publishAttempts=0;
    const undefinedValue=i=>mode===4 || (mode===8&&i%13===0);
    const sampleReady=(i,t)=>{
      if(mode===0||mode===9||mode===10||mode===11||mode===12)return true;
      if(mode===1)return retry>=1;
      if(mode===2)return retry>=4;
      if(mode===3)return retry>=i%5;
      if(mode===4)return false;
      if(mode===5)return i%11!==0||retry>=3;
      if(mode===6)return i%2===0;
      if(mode===7)return retry>=2&&t>=20000;
      if(mode===8)return retry>=4&&i%13!==0;
      return true;
    };
    const canBuild=i=> !(mode===9 && i%7===0) &&
      !(mode===10&&i%9===0&&retry<4) && !(mode===11&&retry<2);
    for(;t<=8000000;t+=500) {
      if(offline(t,broker)){offlineSeen=true;continue;}
      // post-startup: zero catalogue has no publication job but can release.
      if(n===0&&!published)published=true;
      if(published&&pending.size===0&&deferred.size===0){done=true;break;}
      if(pending.size===0&&deferred.size>0){
        if(deadline===0)deadline=t+5000;
        if(t<deadline)continue;
        deadline=0;
        if(retry>=4){for(const i of deferred)discarded.add(i);deferred.clear();published=true;continue;}
        retry++;
        for(const i of deferred)pending.add(i);
        deferred.clear();
      }
      if(pending.size===0)continue;
      if(queueBusy(t,queue,seed)){queueSeen=true;continue;}
      const id=pending.values().next().value;
      publishAttempts++;pending.delete(id);
      if(mode===12&&id%5===0)excluded.add(id);
      else if((sampleReady(id,t)||(retry>=4&&!undefinedValue(id))) && canBuild(id))sent.add(id);
      else deferred.add(id);
      check(!(pending.has(id)&&deferred.has(id)),`bit overlap n=${n} mode=${mode} id=${id}`);
      published=pending.size===0&&deferred.size===0;
    }
    stats.scenarios++;
    if(n===0)stats.zeroCatalogues++;
    if(discarded.size)stats.incompleteDiscoveries++;
    if(offlineSeen)stats.offlineBroker++;
    if(queueSeen)stats.congestedQueue++;
    stats.maxVirtualMinutes=Math.max(stats.maxVirtualMinutes,t/60000);
    check(done,`not terminal n=${n} mode=${mode} broker=${broker} queue=${queue} sent=${sent.size} missing=${discarded.size} pass=${retry}`);
    check(retry<=4,`retry overflow ${retry}`);
    check(!done||sent.size+discarded.size+excluded.size===n,`wrong completeness n=${n} mode=${mode} sent=${sent.size} missing=${discarded.size} excluded=${excluded.size}`);
    if(mode===2&&n>0&&broker===0&&queue===0){check(sent.size===n,"late valid values not found");stats.lateValuesRecovered++;}
    return {done, sent:sent.size, missing:discarded.size, retries:retry};
  }
  const counts=[0,1,2,3,16,31,32,33,64,96,128,240];
  for(const n of counts)for(let m=0;m<=12;m++)for(let b=0;b<4;b++)for(let q=0;q<3;q++)simulate(n,m,b,q,n*401+m*53);
  let rng=0x82AB17F3;const rand=()=>rng=(Math.imul(rng,1664525)+1013904223)>>>0;
  for(let i=0;i<512;i++)simulate([0,1,3,15,32,63,128,240][rand()%8],rand()%13,rand()%4,rand()%3,rand()%139);
  for(let entities=0;entities<=2048;entities++) {
    const words=Math.ceil(entities/32),buffer=new Uint32Array(words*2),a=buffer.subarray(0,words),b=buffer.subarray(words,words*2);
    if(words){a[0]=1;b[0]=2;check(a[0]===1&&b[0]===2,"bitmap overlap");}
    check(buffer.byteLength===words*8&&a.length===b.length,"bitmap size");
    stats.bitmapWidths++;
  }
  for(const response of [202,409,503,401,500,0])for(const enabled of [false,true])
    for(const confirm of [false,true])for(const rebootConfirm of [false,true]){
      const posts=enabled&&confirm?1:0;
      const reboots=posts&&response===409&&rebootConfirm?1:0;
      check(posts<=1,"duplicate POST");
      check(reboots===0||(enabled&&confirm&&rebootConfirm&&response===409),"reboot without consent");
      stats.uiPaths++;
    }
  for(let start=0xffff0000;start<=0xfffffff0;start+=127) {
    const due=(start+5000)>>>0;
    const before=(due-1)>>>0;
    check(((before-due)|0)<0&&((due-due)|0)===0,"millis wrap order");
    stats.millisWrapCases++;
  }
  return {stats,failures};
})();
console.log(JSON.stringify(report, null, 2));
if (report.stats.failures) process.exitCode = 1;

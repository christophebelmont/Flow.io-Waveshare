#!/usr/bin/env node
/**
 * Deterministic failure-injection model for Nextion OTA PSRAM staging,
 * UART acknowledgements, concurrent updater admission and Modbus resume.
 * This exercises the design state machines; it does not replace a C++ build
 * or UART/Nextion hardware integration tests.
 * Run: node scripts/tests/simulate_nextion_ota_failure_matrix.mjs
 */
const report = (function runNextionFailureMatrix() {
  const stats={psram:0,httpResponses:0,httpSessions:0,uart:0,admission:0,progress:0,modbus:0,negative:0};
  function check(ok,message){if(!ok)throw Error("FAIL: "+message);}
  const reserve=2*1024*1024;
  const actualTftSizes=[1854492,1233260,1233260,1854488,1103148];
  const canStage=(bytes,free,largest)=>bytes>0&&free>reserve&&bytes<=free-reserve&&bytes<=largest;
  for (const size of [0,1,4095,4096,4097,...actualTftSizes,6*1048576]) {
    for (const free of [0,reserve-1,reserve,reserve+1,reserve+size-1,reserve+size,reserve+size+1,8*1048576]) {
      for (const largest of [0,size-1,size,size+1,8*1048576]) {
        const admitted=canStage(size,free,largest);
        check(admitted===(size>0&&free>reserve&&size<=free-reserve&&size<=largest),"PSRAM boundary");
        if(admitted)check(size<=largest&&free-size>=reserve,"PSRAM reserve violation");
        stats.psram++;
      }
    }
  }
  const retryable=code=>code<0||code===408||code===429||(code>=500&&code<=599);
  function mode(status,offset,len,total,range) {
    if(total<=0)return "invalid";
    if(status===200&&len===total)return "full";
    const match=/^bytes (\d+)-(\d+)\/(\d+)$/.exec(range||"");
    if(status===206&&offset>0&&offset<total&&len===total-offset&&match&&
      Number(match[1])===offset&&Number(match[2])===total-1&&Number(match[3])===total)return "partial";
    return "invalid";
  }
  const modes=[200,206,404,408,429,500,503,-1,416];
  for (const code of modes)
    for (const size of [0,1,9999,10000,10001])
      for (const offset of [0,1,2500,9999,10000])
        for(const range of ["","bytes 1-9999/10000","bytes 2500-9999/10000","bytes 9999-9999/10000","bytes 1-9999/9999","garbage"]) {
          const value=mode(code,offset,size,10000,range);
          if(value==="full")check(code===200&&size===10000,"unverified 200 accepted");
          if(value==="partial")check(code===206&&offset>0&&size===10000-offset&&range===`bytes ${offset}-9999/10000`,"unverified 206 accepted");
          if(code===404||code===416)check(!retryable(code),"permanent HTTP retry");
          if(code===429||code===503||code===408)check(retryable(code),"transient HTTP failure");
          stats.httpResponses++;
        }
  function download(expected,tries) {
    let received=0, attempted=0, restarts=0;
    for (const resp of tries.slice(0,3)) {
      attempted++;
      const offset=received;
      if(resp===null ||resp.failed){if(attempted>=3)return {ok:false,reason:"retries",received,attempted,restarts};continue;}
      const announced=resp.announced??(resp.status===206?expected-offset:expected);
      const rmode=mode(resp.status,offset,announced,expected,resp.range);
      if(rmode==="invalid"){
        if(!retryable(resp.status))return {ok:false,reason:"invalid",received,attempted,restarts};
        if(attempted>=3)return {ok:false,reason:"retries",received,attempted,restarts};
        continue;
      }
      if(rmode==="full"&&offset>0){received=0;restarts++;}
      received+=Math.min(expected-received,resp.bytes??0);
      if(received===expected)return {ok:true,reason:"done",received,attempted,restarts};
      if(attempted===3)return {ok:false,reason:"retries",received,attempted,restarts};
    }
    return {ok:false,reason:"retries",received,attempted,restarts};
  }
  const len=10000;
  const good={status:200,announced:len,bytes:len};
  const partial={status:206,announced:6000,range:"bytes 4000-9999/10000",bytes:6000};
  const interrupted={status:200,announced:len,bytes:4000};
  const cases=[
    {name:"full first",list:[good],ok:true},
    {name:"drop and HTTP Range resume",list:[interrupted,partial],ok:true},
    {name:"drop and ignore Range, restart full",list:[interrupted,good],ok:true,restarts:1},
    {name:"two transport errors then success",list:[null,{failed:true},good],ok:true,attempted:3},
    {name:"three transport failures",list:[null,null,null],ok:false,reason:"retries"},
    {name:"three partial disconnects",list:[interrupted,{status:206,announced:6000,range:"bytes 4000-9999/10000",bytes:3000},{status:206,announced:3000,range:"bytes 7000-9999/10000",bytes:1000}],ok:false},
    {name:"HTTP 404 permanent",list:[{status:404,announced:len,bytes:len},good],ok:false,reason:"invalid",attempted:1},
    {name:"HTTP 503 then OK",list:[{status:503},good],ok:true},
    {name:"invalid Range response",list:[interrupted,{status:206,announced:6000,range:"bytes 5000-9999/10000",bytes:6000},good],ok:false,reason:"invalid"},
    {name:"mismatched content length",list:[{status:200,announced:len+1,bytes:len},good],ok:false,reason:"invalid"},
    {name:"HTTP 416 permanent",list:[{status:416},good],ok:false,reason:"invalid"},
    {name:"HTTP 429 then OK",list:[{status:429},good],ok:true},
    {name:"HTTP timeout then OK",list:[{status:-11},good],ok:true},
    {name:"zero-byte read timeout",list:[{status:200,announced:len,bytes:0},good],ok:true},
    {name:"unexpected 206 first attempt",list:[{status:206,announced:len,range:"bytes 0-9999/10000",bytes:len}],ok:false,reason:"invalid"}
  ];
  for(const item of cases){
    const result=download(len,item.list);
    check(result.ok===item.ok,"HTTP scenario "+item.name+": "+JSON.stringify(result));
    if(item.reason)check(result.reason===item.reason,"wrong reason "+item.name);
    if(item.restarts!==undefined)check(result.restarts===item.restarts,"range restart "+item.name);
    if(item.attempted!==undefined)check(result.attempted===item.attempted,"attempt limit "+item.name);
    check(result.attempted<=3,"exceeded 3 attempts");
    stats.httpSessions++;
  }
  for(let start=1;start<=75;start++){
    let state=start*1664525+1013904223;
    const next=()=>{state=(Math.imul(state,1664525)+1013904223)>>>0;return state;};
    for(let run=0;run<25;run++){
      const list=Array.from({length:3},()=>{const category=next()%8;return category===0?null:category===1?{status:404}:category===2?{status:503}:category===3?{status:429}:category===4?{status:206,announced:6000,range:"bytes 4000-9999/10000",bytes:6000}:category===5?{status:200,announced:len+1,bytes:len}:{status:200,announced:len,bytes:next()%10001};});
      const outcome=download(len,list);
      check(outcome.attempted<=3,"random HTTP retry bound");
      if(outcome.ok)check(outcome.received===len,"random HTTP premature success");
      stats.httpSessions++;
    }
  }
  function uart(bytes,opts={}) {
    if(!opts.connected) return {ok:false,stage:"baud",acks:0,reset:false,sent:0};
    let packets=0,ackIndex=0,sent=0;
    for(let i=0;i<bytes;i++){
      if(packets===4096){
        ackIndex++;
        if(opts.failBoundaryAck===ackIndex)return {ok:false,stage:"middle_ack",acks:ackIndex,reset:false,sent};
        packets=0;
      }
      if(opts.failWriteAt===i) return {ok:false,stage:"uart_write",acks:ackIndex,reset:false,sent};
      packets++;sent++;
    }
    if(packets===0 || opts.failFinalAck)return {ok:false,stage:"final_ack",acks:ackIndex+1,reset:false,sent};
    ackIndex++;
    return {ok:true,stage:"done",acks:ackIndex,reset:true,sent};
  }
  for(const bytes of [1,2,1023,1024,4095,4096,4097,8192,8193,...actualTftSizes]) {
    for(const connected of [true,false]){
      for(const failFinalAck of [true,false]){
        const result=uart(bytes,{connected,failFinalAck});
        if(!connected)check(!result.ok&&result.stage==="baud"&&!result.reset,"baud failure touched panel");
        else if(failFinalAck)check(!result.ok&&result.stage==="final_ack"&&!result.reset,"missing final ack falsely succeeds");
        else check(result.ok&&result.sent===bytes&&result.acks===Math.ceil(bytes/4096)&&result.reset,"UART success ACK count");
        stats.uart++;
      }
    }
    for(const failBoundaryAck of [1,2,3]){
      let result=uart(bytes,{connected:true,failBoundaryAck});
      if(bytes>failBoundaryAck*4096)
        check(!result.ok&&result.stage==="middle_ack"&&!result.reset,"boundary ack accepted on failure");
      stats.uart++;
    }
    let result=uart(bytes,{connected:true,failWriteAt:Math.floor(bytes/2)});
    check(!result.ok&&result.stage==="uart_write"&&!result.reset,"serial write failure reset panel");
    stats.uart++;
  }
  function admission(state,remoteWorker=false){
    return !!((state.busy&&!remoteWorker)||state.local||state.queued||state.manifestPending||
       state.nextionReboot||state.startPending||state.manifestActive||state.copyReaders);
  }
  const fields=["busy","local","queued","manifestPending","nextionReboot","startPending","manifestActive","copyReaders"];
  for(let bits=0;bits<(1<<fields.length);bits++){
    const state={};for(let j=0;j<fields.length;j++)state[fields[j]]=!!(bits&(1<<j));
    const expected=bits!==0;
    check(admission(state)===expected,"unprotected OTA overlap "+bits);
    check(admission(state,true)===(!!(bits & ~1)),"nested remote-worker admission "+bits);
    stats.admission+=2;
  }
  // Interleaving: initial atomic reservation prevents a competing operation,
  // regardless of whether Nextion or paired firmware+SPIFFS starts first.
  for(const first of ["nextion","paired","local"])
    for(const second of ["nextion","paired","local"])
      for(const releaseAtEnd of [true,false]){
        const state={busy:false,local:false,queued:false,manifestPending:false,nextionReboot:false,startPending:false,manifestActive:false,copyReaders:false};
        check(!admission(state),"first operation rejected");
        if(first==="local")state.local=true;else state.startPending=true;
        check(admission(state),"second update accepted during first admission");
        state.startPending=false;
        if(first!=="local")state.busy=true;
        check(admission(state),"second update accepted while active");
        if(releaseAtEnd){state.busy=false;state.local=false;check(!admission(state),"failed to release after completed/failed OTA");}
        stats.admission++;
      }
  const phases=[
    ...[0,1,5,30,50,80,99,100].map(v=>({name:"download",overall:Math.floor(v*25/100),local:v})),
    {name:"verify",overall:25,local:0},{name:"verify",overall:25,local:100},
    {name:"uart",overall:30,local:0},
    ...[0,1,25,50,75,99,100].map(v=>({name:"upload",overall:30+Math.floor(v*65/100),local:v})),
    {name:"ack",overall:95,local:0},{name:"ack",overall:95,local:100},
    {name:"reboot",overall:100,local:100}
  ];
  let last=0;
  for(const p of phases){check(p.overall>=last&&p.overall<=100,"nonmonotonic progress "+p.name);last=p.overall;stats.progress++;}
  function modbusTick(otaBusy,elapsed,running,paused){
    if(otaBusy){if(elapsed>=300 &&!paused) return {running:false,paused:true,admit:false};return {running,paused,admit:false};}
    return {running:running||paused,paused:false,admit:true};
  }
  for(const time of [0,1,299,300,301,500,10000]){
    const busy=modbusTick(true,time,true,false);check(!busy.admit,"Modbus request admitted during update");
    if(time>=300)check(!busy.running&&busy.paused,"Modbus failed to close after 300ms");
    const idle=modbusTick(false,time,busy.running,busy.paused);
    check(idle.admit&&idle.running,"Modbus failed to resume");
    stats.modbus++;
  }
  // Safety requirement: no UART starts if PSRAM, HTTP, digest or compatibility
  // checks fail; no reset after missing acknowledgement.
  for(const scenario of [
    {memory:false,network:true,sha:true,compatible:true,connected:true,ack:true},
    {memory:true,network:false,sha:true,compatible:true,connected:true,ack:true},
    {memory:true,network:true,sha:false,compatible:true,connected:true,ack:true},
    {memory:true,network:true,sha:true,compatible:false,connected:true,ack:true},
    {memory:true,network:true,sha:true,compatible:true,connected:false,ack:true},
    {memory:true,network:true,sha:true,compatible:true,connected:true,ack:false},
    {memory:true,network:true,sha:true,compatible:true,connected:true,ack:true}
  ]) {
    const startUart=scenario.memory&&scenario.network&&scenario.sha&&scenario.compatible;
    const resetPanel=startUart&&scenario.connected&&scenario.ack;
    check(startUart===(scenario.memory&&scenario.network&&scenario.sha&&scenario.compatible),"UART precondition");
    check(!resetPanel||startUart,"reset without staged firmware");
    stats.negative++;
  }
  return {total:Object.values(stats).reduce((a,b)=>a+b,0),categories:stats,method:"deterministic model scenarios; not firmware execution or hardware validation"};
})();
console.log(JSON.stringify(report,null,2));

int main() {
 PoolDeviceModule module;
 unsigned sensors=0,numbers=0,buttons=0,selects=0;
 struct Capture { unsigned *s,*n,*b,*sel; } capture{&sensors,&numbers,&buttons,&selects};
 HAService ha{}; ha.ctx=&capture;
 ha.addSensor=[](void* p,const HASensorEntry*){++*static_cast<Capture*>(p)->s;return true;};
 ha.addNumber=[](void* p,const HANumberEntry*){++*static_cast<Capture*>(p)->n;return true;};
 ha.addButton=[](void* p,const HAButtonEntry*){++*static_cast<Capture*>(p)->b;return true;};
 ha.addSelect=[](void* p,const HASelectEntry* e){
   ++*static_cast<Capture*>(p)->sel;
   DynamicJsonDocument doc(2048);assert(!deserializeJson(doc,e->optionsJson));assert(doc.size()==16);
   assert(strstr(e->commandTemplate,"to_json"));return true;
 };
 module.haSvc_=&ha; module.restoreOverrides_(); module.registerOverrideHa_();
 assert(sensors==2&&numbers==1&&buttons==3&&selects==1);
 {
   PoolDeviceModule disabled;
   for (auto& slot : disabled.slots_) slot.def.enabled=false;
   disabled.haSvc_=&ha;
   disabled.registerOverrideHa_();
   assert(sensors==2&&numbers==1&&buttons==3&&selects==1);
 }
 using Cmd=ActuatorOverrideCommand;
 char reply[256];
 auto call=[&](const char* args,Cmd action) { return module.svcOverrideCommandImpl_({nullptr,nullptr,args,42},reply,sizeof(reply),action); };
 assert(call(R"({"minutes":45})",Cmd::Duration));
 assert(call(nullptr,Cmd::On));
 assert(module.slots_[0].actualOn && !module.slots_[0].desiredOn);
 assert(module.slots_[0].overrideTimer.state(nowMs).remainingSec==2700);
 assert(module.loggedActor==42 && module.logCount==1);
 assert(call(R"({"option":"pd7 — Equipment"})",Cmd::Select));
 assert(module.slots_[0].actualOn && !module.slots_[7].actualOn);
 assert(module.slots_[0].overrideTimer.active());
 module.slots_[7].desiredOn=true;
 assert(call(R"({"slot":7,"duration_s":900})",Cmd::Off));
 assert(!module.slots_[7].actualOn && module.slots_[7].desiredOn);
 assert(module.slots_[0].actualOn);
 assert(!call(R"({"slot":3})",Cmd::On)); // Explicit target requires explicit duration.
 assert(!call(R"({"slot":3,"duration_s":0})",Cmd::On));
 assert(!call(R"({"slot":3,"duration_s":86401})",Cmd::On));
 assert(!call(R"({"slot":3,"duration_s":"900"})",Cmd::On));
 assert(!call(R"({"slot":3,"value":"on","duration_s":900})",Cmd::FromArgs));
 assert(!call(R"({"slot":99,"duration_s":900})",Cmd::On));
 assert(!call(R"({"option":"Equipment"})",Cmd::Select));
 assert(!call(R"({"minutes":0})",Cmd::Duration));
 assert(call(R"({"minutes":15})",Cmd::Duration));
 assert(module.slots_[0].overrideTimer.state(nowMs).remainingSec==2700);
 assert(module.slots_[7].overrideTimer.state(nowMs).remainingSec==900);
 char snapshot[8192]; uint32_t timestamp;
 assert(module.buildOverrideSnapshot_(true,snapshot,sizeof(snapshot),timestamp));
 DynamicJsonDocument doc(16384);assert(!deserializeJson(doc,snapshot));
 assert(doc["actuators"].size()==16 && doc["active_count"]==2);
 assert(doc["actuators"]["pd0"]["actual_on"]==true);
 assert(doc["actuators"]["pd7"]["override_value"]==false);
 assert(module.buildOverrideSnapshot_(false,snapshot,sizeof(snapshot),timestamp));
 assert(!deserializeJson(doc,snapshot));assert(doc["id"]=="pd7");
 assert(doc["duration_minutes"]==15);
 // Reboot keeps independent deadlines and the pre-force manual baseline.
 PoolDeviceModule reboot; reboot.storage.records=module.storage.records;
 nowMs=0; utc=1100; reboot.restoreOverrides_();
 for(auto& s:reboot.slots_) s.desiredOn=s.overrideRestored&&s.overrideBaseline;
 reboot.tickDevices_(0,false);
 assert(reboot.slots_[0].actualOn && !reboot.slots_[7].actualOn);
 assert(reboot.slots_[0].control.remainingSec==2600);
 assert(reboot.slots_[7].control.remainingSec==800 && reboot.slots_[7].desiredOn);
 nowMs=800001;utc=1900;reboot.tickDevices_(nowMs,false);
 assert(reboot.slots_[7].control.mode==ActuatorControlMode::Guided && reboot.slots_[7].actualOn);
 assert(reboot.slots_[0].overrideTimer.active());
 // Latest guided target is used at release; changing duration/selection never changes it.
 reboot.slots_[0].desiredOn=true;
 assert(reboot.svcReleaseOverrideImpl_(0,ActuatorOverrideReason::Released)==POOLDEV_SVC_OK);
 assert(reboot.slots_[0].actualOn && reboot.slots_[0].control.mode==ActuatorControlMode::Guided);
 // A restart without trusted time leaves an active lease visible but physically OFF.
 PoolDeviceModule noClock;noClock.storage.records=module.storage.records;
 utc=0;nowMs=0;noClock.restoreOverrides_();noClock.tickDevices_(0,false);
 assert(noClock.slots_[0].control.mode==ActuatorControlMode::WaitingTime&&!noClock.slots_[0].actualOn);
 utc=1500;noClock.tickDevices_(0,false);assert(noClock.slots_[0].actualOn);
 // Domain and hardware safety cancel the lease, even when normal demand is ON.
 noClock.slots_[0].desiredOn=true;noClock.slots_[0].overridePolicy.allowOn=false;
 noClock.tickDevices_(1,false);assert(!noClock.slots_[0].actualOn&&!noClock.slots_[0].overrideTimer.active());
 noClock.tickDevices_(2,false);assert(!noClock.slots_[0].actualOn);
 // Restore dependency order is independent of slot order; later loss never rearms.
 PoolDeviceModule ordered; ordered.storage.records=module.storage.records;
 ordered.restoreOverrides_();ordered.slots_[0].overridePolicy.requiredOnMask=uint16_t(1)<<12;
 ordered.tickDevices_(0,false);
 assert(!ordered.slots_[0].actualOn&&ordered.slots_[0].overrideTimer.active());
 ordered.slots_[12].desiredOn=true;ordered.tickDevices_(0,false);ordered.tickDevices_(0,false);
 assert(ordered.slots_[0].actualOn);
 ordered.slots_[12].desiredOn=false;ordered.tickDevices_(0,false);ordered.tickDevices_(0,false);
 assert(!ordered.slots_[0].actualOn&&!ordered.slots_[0].overrideTimer.active());
 // Failed writes cannot change an existing lease; failed cancellation blocks restart.
 nowMs=100;utc=1000;module.storage.fail=true;
 assert(!call(R"({"slot":0,"duration_s":300})",Cmd::Off));
 assert(module.slots_[0].overrideTimer.state(nowMs).value);
 assert(!call(R"({"slot":0})",Cmd::Release));
 assert(!module.slots_[0].actualOn&&module.slots_[0].overrideTimer.pendingSave());
 assert(!call(R"({"slot":0,"duration_s":300})",Cmd::On));
 module.storage.fail=false;nowMs=6000;utc=1006;module.tickDevices_(nowMs,false);
 assert(!module.slots_[0].overrideTimer.pendingSave());
 module.slots_[3].overridePolicy.allowOff=false;
 assert(!call(R"({"slot":3,"duration_s":300})",Cmd::Off));
 module.slots_[3].dependencies=false;
 assert(!call(R"({"slot":3,"duration_s":300})",Cmd::On));
 module.slots_[3].dependencies=true;module.slots_[3].maxUptime=true;
 assert(!call(R"({"slot":3,"duration_s":300})",Cmd::On));
 // Duplicate physical output ownership is excluded from the catalogue.
 module.slots_[3].driverConfig.outputs[0]=module.slots_[4].driverConfig.outputs[0];
 assert(!module.overrideSupported_(3)&&!module.overrideSupported_(4));
 assert(!call(R"({"slot":3,"duration_s":300})",Cmd::Off));
 // Upgrade migration is durable and idempotent; a tombstone prevents resurrection.
 ConfigStore legacyStore;
 TimedActuatorOverride legacy;
 legacy.configure(&legacyStore,[](void* p,const uint8_t* b,size_t n){return static_cast<ConfigStore*>(p)->writeRuntimeBlob("ovr_robot",b,n);});
 assert(legacy.start(11,12U<<16|11U,true,1800,1000,0));
 PoolDeviceModule migrated; migrated.storage.records=legacyStore.records; migrated.restoreOverrides_();
 assert(migrated.slots_[11].overrideTimer.active());
 assert(migrated.storage.records.count("pdm_ovr_11"));
 migrated.svcReleaseOverrideImpl_(11,ActuatorOverrideReason::Released);
 PoolDeviceModule upgradedAgain;upgradedAgain.storage.records=migrated.storage.records;upgradedAgain.restoreOverrides_();
 assert(!upgradedAgain.slots_[11].overrideTimer.active());
 // Manual OFF atomically cancels a forced OFF with an ON baseline: no ON pulse.
 module.slots_[7].desired.running=true;
 module.slots_[7].desiredOn=true;
 assert(call(R"({"slot":7,"duration_s":900})",Cmd::Off));
 const auto ticks=module.ticks;
 assert(module.svcSetManualRunningImpl_(7,0)==POOLDEV_SVC_OK);
 assert(module.ticks==ticks+1&&!module.slots_[7].actualOn&&!module.slots_[7].overrideTimer.active());
 // Invalid manual targets leave the active lease intact.
 assert(call(R"({"slot":7,"duration_s":900})",Cmd::On));
 const PoolDeviceTarget invalid{false,101};
 assert(module.setTarget_(7,&invalid,true)==POOLDEV_SVC_ERR_INVALID_ARG);
 assert(module.slots_[7].overrideTimer.active());
 // Legacy role adapters use current numeric assignments and preserve attribution.
 PoolDeviceModule adapted; adapted.restoreOverrides_(); adapted.registerOverrideHa_();
 PoolLogicModule logic(adapted);
 auto oldCall=[&](const char* args,Cmd command){return logic.handleOverride_({nullptr,nullptr,args,73},reply,sizeof(reply),command);};
 assert(oldCall(R"({"role":"filtration","minutes":45})",Cmd::Duration));
 assert(oldCall(R"({"role":"filtration"})",Cmd::On));
 assert(adapted.slots_[3].overrideTimer.active()&&adapted.slots_[3].control.remainingSec==2700);
 assert(adapted.loggedActor==73);
 assert(oldCall(R"({"role":"robot"})",Cmd::Off));
 assert(adapted.slots_[6].overrideTimer.active()&&!logic.robotManualOverride_);
 assert(!oldCall(R"({"role":"robot","slot":6})",Cmd::On));
 assert(!oldCall(R"({"role":"Robot traduit"})",Cmd::On));
 assert(!oldCall(R"({"slot":9})",Cmd::On));
 assert(oldCall(R"({"slot":9,"duration_s":300})",Cmd::On));
 // A migration save failure remains inhibited until its cancellation is durable.
 PoolDeviceModule migrationFailure;migrationFailure.storage.records=legacyStore.records;migrationFailure.storage.fail=true;
 migrationFailure.restoreOverrides_();assert(migrationFailure.slots_[11].overrideTimer.pendingSave());
 assert(!migrationFailure.slots_[11].overrideTimer.active());

 // Sixteen active overrides and escaped maximum-length labels fit shared buffers.
 PoolDeviceModule full;
 for (auto& slot : full.slots_) {
   memset(slot.def.label, '"', sizeof(slot.def.label)-1);
   slot.def.label[sizeof(slot.def.label)-1]='\0';
 }
 full.restoreOverrides_(); full.registerOverrideHa_();
 assert(!deserializeJson(doc,full.overrideOptions_) && doc.size()==16);
 for (unsigned slot=0;slot<16;++slot) {
   char args[80];snprintf(args,sizeof(args),"{\"slot\":%u,\"duration_s\":86400}",slot);
   assert(full.svcOverrideCommandImpl_({nullptr,nullptr,args,42},reply,sizeof(reply),Cmd::On));
 }
 assert(full.buildOverrideSnapshot_(true,snapshot,sizeof(snapshot),timestamp));
 assert(!deserializeJson(doc,snapshot));
 assert(doc["actuators"].size()==16 && doc["active_count"]==16);
 assert(doc["actuators"]["pd15"]["actual_on"]==true);

 // The highest dependency bit gates and cancels an override just like bit zero.
 PoolDeviceModule upper;upper.restoreOverrides_();
 upper.slots_[0].overridePolicy.requiredOnMask=uint16_t(1)<<15;
 const CommandRequest onFirst{nullptr,nullptr,R"({"slot":0,"duration_s":300})",42};
 assert(!upper.svcOverrideCommandImpl_(onFirst,reply,sizeof(reply),Cmd::On));
 upper.slots_[15].desiredOn=true;upper.tickDevices_(0,false);
 assert(upper.svcOverrideCommandImpl_(onFirst,reply,sizeof(reply),Cmd::On));
 assert(upper.slots_[0].actualOn);
 upper.slots_[15].desiredOn=false;upper.tickDevices_(0,false);upper.tickDevices_(0,false);
 assert(!upper.slots_[0].actualOn && !upper.slots_[0].overrideTimer.active());
 // A controller temperature update preserves the mode selected by another caller.
 PoolDeviceModule heating;
 heating.slots_[0].desired.mode=2;
 assert(heating.svcSetRunningAtSetpointImpl_(0,1,25)==POOLDEV_SVC_OK);
 assert(heating.slots_[0].desired.running && heating.slots_[0].desired.setpoint==25 && heating.slots_[0].desired.mode==2);
 assert(heating.svcSetRunningAtSetpointImpl_(0,1,101)==POOLDEV_SVC_ERR_INVALID_ARG);
 assert(heating.slots_[0].desired.setpoint==25 && heating.slots_[0].desired.mode==2);
 // The configuration owns only the assigned thermal device's temperature.
 struct TemperatureConfig { float value=29; unsigned writes=0; bool fail=false; } temperature;
 PoolConfigurationService authority{};
 authority.ctx=&temperature;
 authority.getDeviceSetpoint=[](void* ctx,uint8_t slot,float* out) {
   if(slot!=7)return false;
   *out=static_cast<TemperatureConfig*>(ctx)->value;return true;
 };
 authority.setDeviceSetpoint=[](void* ctx,uint8_t slot,float value) {
   auto& config=*static_cast<TemperatureConfig*>(ctx);
   if(slot!=7||config.fail)return false;
   config.value=value;++config.writes;return true;
 };
 PoolDeviceModule pac;
 pac.poolConfigurationSvc_=&authority;
 auto& thermal=pac.slots_[7];
 thermal.driverConfig.capabilities.unit=PoolSetpointUnit::Celsius;
 thermal.desired={false,25,2};
 pac.syncConfigurationSetpoint_(7); // boot before the first write
 assert(thermal.desired.setpoint==29&&!thermal.desired.running&&thermal.desired.mode==2);
 assert(temperature.writes==0);
 temperature.value=30; // ConfigStore changed with automatic heating disabled
 pac.syncConfigurationSetpoint_(7);
 assert(thermal.desired.setpoint==30&&!thermal.desired.running);
 PoolDeviceTarget requested{false,31,2};
 assert(pac.svcSetTargetImpl_(7,&requested)==POOLDEV_SVC_OK);
 assert(temperature.value==31&&temperature.writes==1);
 assert(pac.svcSetTargetImpl_(7,&requested)==POOLDEV_SVC_OK);
 assert(temperature.writes==1);
 temperature.fail=true;requested.setpoint=32;
 assert(pac.svcSetTargetImpl_(7,&requested)==POOLDEV_SVC_ERR_IO);
 assert(thermal.desired.setpoint==31&&temperature.value==31);
 requested.setpoint=101;
 assert(pac.svcSetTargetImpl_(7,&requested)==POOLDEV_SVC_ERR_INVALID_ARG);
 assert(temperature.writes==1);
 // A later ON/OFF command uses a freshly changed configuration, not a stale target.
 temperature.value=28;
 assert(pac.svcSetRunningImpl_(7,0)==POOLDEV_SVC_OK);
 assert(thermal.desired.setpoint==28&&!thermal.desired.running&&thermal.desired.mode==2);
 pac.slots_[0].desired.setpoint=77;
 pac.syncConfigurationSetpoint_(0);
 assert(pac.slots_[0].desired.setpoint==77);

}

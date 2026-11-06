#pragma once
#include <ArduinoJson.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include <map>
#include <vector>
#include "Core/Control/TimedActuatorOverride.h"
#include "Core/Control/ActuatorOverrideDuration.h"
#include "Core/Services/IHA.h"
#include "Core/Services/IPoolConfiguration.h"
#include "Core/Services/PoolSerialTelemetry.h"
using SpiRamJsonDocument = DynamicJsonDocument;
constexpr uint8_t POOL_DEVICE_MAX=16, POOL_MAX_SPEED_STEPS=6;
constexpr int IO_ID_INVALID=-1;
enum class PoolControlKind { Relay, Discrete, Rs485 };
enum class PoolSetpointUnit { Percent, Celsius };
enum class PoolSerialModel { Custom, Polytropic };
enum class PoolCommandPhase { Applied, Expired };
enum class PoolFeedbackQuality { Good, Stale };
enum class PoolInterlockState { Ready, StartRejected };
enum class PoolDeviceSvcStatus { OK, InvalidArg, UnknownSlot, NotReady, IoError, Disabled, MaxUptime, Interlock };
constexpr auto POOLDEV_SVC_OK=PoolDeviceSvcStatus::OK;
constexpr auto POOLDEV_SVC_ERR_INVALID_ARG=PoolDeviceSvcStatus::InvalidArg;
constexpr auto POOLDEV_SVC_ERR_UNKNOWN_SLOT=PoolDeviceSvcStatus::UnknownSlot;
constexpr auto POOLDEV_SVC_ERR_NOT_READY=PoolDeviceSvcStatus::NotReady;
constexpr auto POOLDEV_SVC_ERR_IO=PoolDeviceSvcStatus::IoError;
constexpr auto POOLDEV_SVC_ERR_DISABLED=PoolDeviceSvcStatus::Disabled;
constexpr auto POOLDEV_SVC_ERR_MAX_UPTIME=PoolDeviceSvcStatus::MaxUptime;
constexpr auto POOLDEV_SVC_ERR_INTERLOCK=PoolDeviceSvcStatus::Interlock;
struct PoolDeviceTarget { bool running=false; float setpoint=100; uint8_t mode=0; };
template<class T> bool validatePoolTarget(const T&,const PoolDeviceTarget& target){return target.setpoint>=0&&target.setpoint<=100;}
enum class ErrorCode { MissingArgs, BadSlot, BadCmdJson, Failed, Disabled, NotReady, InterlockBlocked };
void writeErrorJson(char* out, size_t len, ErrorCode, const char*) { snprintf(out,len,"{\"ok\":false}"); }
struct CommandRequest { const char* cmd=nullptr; const char* json=nullptr; const char* args=nullptr; int actor=42; };
enum class ActivityCode { PoolLogicOverrideOnRequested, PoolLogicOverrideOffRequested };
enum class ActivityRole { None };
enum class ActivitySource { Manual };
enum class ActivitySeverity { Info };
enum class ActivityState { RequestedOn, RequestedOff };
enum class ActivityReason { Manual };
constexpr size_t ACTIVITY_TITLE_MAX=100, ACTIVITY_DETAIL_MAX=256;
namespace DataKeys { constexpr int PoolDeviceOverrides=13; }
namespace NvsKeys { namespace PoolDevice { constexpr auto OverrideDuration="pdm_ovr_min"; } }
struct Store { int notifications=0; void notifyChanged(int) { ++notifications; } };
struct ConfigStore {
 bool fail=false; unsigned writes=0;
 std::map<std::string,std::vector<uint8_t>> records;
 bool writeRuntimeBlob(const char* key,const uint8_t* bytes,size_t len) {
   ++writes; if(fail)return false; records[key]={bytes,bytes+len}; return true;
 }
 bool readRuntimeBlob(const char* key,uint8_t* out,size_t cap,size_t* len) {
   auto it=records.find(key); if(it==records.end())return false;
   *len=it->second.size(); memcpy(out,it->second.data(),std::min(cap,*len)); return true;
 }
};
uint64_t nowMs=100, utc=1000;
uint32_t millis(){return uint32_t(nowMs);}
uint64_t esp_timer_get_time(){return nowMs*1000;}
enum class TimeQuality { RtcTrusted, NtpSynced, Unknown };
struct TimeState { bool valid=true; TimeQuality quality=TimeQuality::RtcTrusted; uint64_t currentTimeUtc=utc; };
struct TimeService {
 void* ctx=nullptr;
 bool (*currentState)(void*,TimeState*)=[](void*,TimeState* s){s->currentTimeUtc=utc;return true;};
};
struct IoEndpointMeta { uint16_t bindingPort=1; };
struct IoService {
 void* ctx=nullptr;
 void (*meta)(void*,int,IoEndpointMeta*)=[](void*,int id,IoEndpointMeta* m){m->bindingPort=id+1;};
 void (*setOutputControlState)(void*,int,int,const ActuatorControlState*)=nullptr;
};
#define LOGE(...) ((void)0)
#define LOGI(...) ((void)0)
class PoolDeviceModule {
public:
 struct PoolDeviceSlot {
  bool used=true;
  std::string idStorage;
  const char* id=nullptr;
  struct { bool enabled=true; char label[24]="Equipment"; } def;
  struct { struct { PoolControlKind kind=PoolControlKind::Relay; PoolSetpointUnit unit=PoolSetpointUnit::Percent; } capabilities;
     struct { PoolSerialModel model=PoolSerialModel::Custom; } serial;
     int outputs[POOL_MAX_SPEED_STEPS]={IO_ID_INVALID,IO_ID_INVALID,IO_ID_INVALID,IO_ID_INVALID,IO_ID_INVALID,IO_ID_INVALID}; } driverConfig;
  bool driverReady=true, desiredOn=false, actualOn=false, dependencies=true, maxUptime=false;
  struct { bool error=false; PoolFeedbackQuality quality=PoolFeedbackQuality::Good; PoolCommandPhase phase=PoolCommandPhase::Applied; PoolTelemetryState telemetry{}; } feedback;
  PoolDeviceTarget desired, effective;
  bool retryExpiredCommand=false;
  uint32_t revision=0;
  PoolInterlockState interlockState=PoolInterlockState::Ready;
  ActuatorControlState control{};
  ActuatorOverridePolicy overridePolicy{};
  TimedActuatorOverride overrideTimer;
  PoolDeviceModule* owner=nullptr;
  char overrideKey[16]{}, overrideOption[48]{};
  bool overrideBaseline=false, overrideRestored=false;
  uint64_t overrideRetryMs=0;
 } slots_[POOL_DEVICE_MAX];
 const PoolConfigurationService* poolConfigurationSvc_=nullptr;
 bool configurationSetpoint_(uint8_t,float&) const;
 void syncConfigurationSetpoint_(uint8_t);
 ConfigStore storage; ConfigStore* cfgStore_=&storage;
 TimeService time; TimeService* timeSvc_=&time;
 IoService io; IoService* ioSvc_=&io;
 Store store; Store* dataStore_=&store;
 const HAService* haSvc_=nullptr;
 bool runtimeReady_=true, writesEnabled_=true;
 int loggedActor=0, logCount=0, ticks=0;
 ActuatorOverrideDuration overrideDuration_;
 uint8_t overrideSelection_=0xFF;
 uint32_t overridePublishMs_=0;
 char overrideOptions_[1024]{};
 PoolDeviceModule() {
  for(unsigned i=0;i<POOL_DEVICE_MAX;++i) {
   slots_[i].idStorage="pd"+std::to_string(i);slots_[i].id=slots_[i].idStorage.c_str();
   slots_[i].driverConfig.outputs[0]=int(i);
  }
 }
 bool lockState_() const {return true;}
 void unlockState_() const {}
 bool dependenciesSatisfied_(uint8_t slot, bool = false) const {return slots_[slot].dependencies;}
 static bool maxUptimeReached_(const PoolDeviceSlot& s){return s.maxUptime;}
 void tickDevices_(uint32_t,bool) { ++ticks; for(uint8_t i=0;i<POOL_DEVICE_MAX;++i) if(slots_[i].used) slots_[i].actualOn=resolveOverride_(i, slots_[i].dependencies&&!slots_[i].maxUptime, nowMs); }
 void emitActivity_(ActivityCode, ActivitySource, ActivitySeverity, ActivityRole, ActivityState,
                    ActivityReason, uint8_t, const char*, const char*, const char*, int actor) { loggedActor=actor; ++logCount; }
 PoolDeviceSvcStatus svcSetRunningImpl_(uint8_t,uint8_t);
 PoolDeviceSvcStatus svcSetManualRunningImpl_(uint8_t,uint8_t);
 PoolDeviceSvcStatus setRunning_(uint8_t,uint8_t,bool,const float* = nullptr);
 PoolDeviceSvcStatus svcSetRunningAtSetpointImpl_(uint8_t,uint8_t,float);
 PoolDeviceSvcStatus svcSetTargetImpl_(uint8_t,const PoolDeviceTarget*);
 PoolDeviceSvcStatus setTarget_(uint8_t,const PoolDeviceTarget*,bool);
 bool overrideSupported_(uint8_t) const;
 uint32_t overrideBinding_(uint8_t) const;
 bool overrideDependenciesSatisfied_(uint8_t) const;
 uint64_t overrideUtc_() const;
 static bool saveOverride_(void*,const uint8_t*,size_t);
 static bool saveOverrideDuration_(void*,const uint8_t*,size_t);
 void restoreOverrides_();
 void registerOverrideHa_();
 PoolDeviceSvcStatus svcSetOverridePoliciesImpl_(const ActuatorOverridePolicy*,uint8_t);
 PoolDeviceSvcStatus svcReleaseOverrideImpl_(uint8_t,ActuatorOverrideReason);
 uint16_t svcOverrideDurationImpl_() const;
 bool resolveOverride_(uint8_t,bool,uint64_t);
 static bool cmdOverride_(void*,const CommandRequest&,char*,size_t);
 static bool cmdOverrideOn_(void*,const CommandRequest&,char*,size_t);
 static bool cmdOverrideOff_(void*,const CommandRequest&,char*,size_t);
 static bool cmdOverrideRelease_(void*,const CommandRequest&,char*,size_t);
 static bool cmdOverrideDuration_(void*,const CommandRequest&,char*,size_t);
 static bool cmdOverrideSelect_(void*,const CommandRequest&,char*,size_t);
 bool svcOverrideCommandImpl_(const CommandRequest&,char*,size_t,ActuatorOverrideCommand);
 bool buildOverrideSnapshot_(bool,char*,size_t,uint32_t&) const;
};

#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
constexpr DataKey DATAKEY_IO_BASE = DataKeys::IoBase;
constexpr DataKey DATAKEY_POOL_DEVICE_STATE_BASE = DataKeys::PoolDeviceStateBase;
constexpr uint16_t IO_MAX_ENDPOINTS = Limits::Io::MaxEndpoints;
class PoolLogicModule {
public:
 using OverrideCommand=ActuatorOverrideCommand;
 struct OverrideLock { explicit OverrideLock(int){} };
 int overrideMutex_=0,pendingMux_=0;
 uint8_t filtrationDeviceSlot_=3,robotDeviceSlot_=6;
 bool robotManualOverride_=true,robotManualDesired_=true;
 struct Adapter {
  void* ctx;
  bool (*overrideCommand)(void*,const CommandRequest&,char*,size_t,ActuatorOverrideCommand)=
   [](void* p,const CommandRequest& r,char* o,size_t n,ActuatorOverrideCommand c){return static_cast<PoolDeviceModule*>(p)->svcOverrideCommandImpl_(r,o,n,c);};
  uint16_t (*overrideDuration)(void*)=[](void* p){return static_cast<PoolDeviceModule*>(p)->svcOverrideDurationImpl_();};
 } adapter;
 Adapter* poolSvc_=&adapter;
 explicit PoolLogicModule(PoolDeviceModule& module):adapter{&module}{}
 bool handleOverride_(const CommandRequest&,char*,size_t,OverrideCommand);
 bool runtimeSnapshotAffectsKey(uint16_t, DataKey) const;
};

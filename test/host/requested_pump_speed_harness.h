#include <cassert>
#include <map>
#include <string>
#include "Core/ConfigCandidate.h"
#include "Core/SpiRamObject.h"
#include "Modules/PoolDeviceModule/Drivers/PoolDeviceDriver.h"
#include "Modules/PoolDeviceModule/Drivers/PoolDriverConfig.h"
#include "Modules/PoolDeviceModule/Drivers/PoolSerialProfiles.h"
#include "Core/Services/IPoolConfiguration.h"

uint32_t testNowMs = 0;
uint32_t millis() { return testNowMs; }
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)

// Only storage and external services are faked; ConfigStore's typed mutation
// and the PoolDevice persistence/command/validation functions are production code.
struct Preferences {
    std::map<std::string, float> rpm;
    float getFloat(const char* key, float fallback) { auto i=rpm.find(key); return i==rpm.end()?fallback:i->second; }
    int32_t getInt(const char*, int32_t fallback) { return fallback; }
    uint16_t getUShort(const char*, uint16_t fallback) { return fallback; }
    uint8_t getUChar(const char*, uint8_t fallback) { return fallback; }
    bool getBool(const char*, bool fallback) { return fallback; }
    void getBytes(const char*, void*, size_t) {}
};
struct ConfigStore {
    Preferences prefs;
    Preferences* _prefs=&prefs;
    bool fail=false;
    unsigned writes=0;
    std::map<const void*, const char*> keys;
    struct MutationGuard { explicit MutationGuard(int) {} };
    int mutationMutex_=0;
    bool (*validator)(void*, const ConfigCandidate&)=nullptr;
    void* context=nullptr;
    template<typename T, size_t H> bool set(ConfigVariable<T,H>&, const T&);
    template<typename T, size_t H> void loadPersistentVar(ConfigVariable<T,H>&);
    bool validateCandidate(const ConfigCandidate& c) { return !validator || validator(context,c); }
    bool lockPrefs_() { return !fail; }
    void unlockPrefs_() {}
    bool putFloat_(const char* key, float value) { if(fail)return false; ++writes; prefs.rpm[key]=value; return true; }
    bool putInt_(const char*, int32_t) { return false; }
    bool putUShort_(const char*, uint16_t) { return false; }
    bool putUChar_(const char*, uint8_t) { return false; }
    bool putBool_(const char*, bool) { return false; }
    bool putBytes_(const char*, const void*, size_t) { return false; }
    void notifyChanged(const char*,const char*,uint8_t,uint8_t) {}
    bool persistentKeyExists(const char* key, bool& exists) { exists=prefs.rpm.count(key); return !fail; }
    bool persistRegisteredValue(const void* value) { return putFloat_(keys.at(value), *static_cast<const float*>(value)); }
};
constexpr uint8_t POOL_DEVICE_MAX=2;
enum class PoolInterlockState { Ready, StartRejected };
enum PoolDeviceSvcStatus { POOLDEV_SVC_OK, POOLDEV_SVC_ERR_INVALID_ARG, POOLDEV_SVC_ERR_UNKNOWN_SLOT,
    POOLDEV_SVC_ERR_NOT_READY, POOLDEV_SVC_ERR_DISABLED, POOLDEV_SVC_ERR_MAX_UPTIME,
    POOLDEV_SVC_ERR_INTERLOCK, POOLDEV_SVC_ERR_IO };
class PoolDeviceModule {
public:
    struct Slot {
        bool used=true, driverReady=true, desiredOn=false, retryExpiredCommand=false, bootResumePending=false;
        struct { bool enabled=true; } def;
        PoolDriverConfig driverConfig;
        char driverJson[POOL_DRIVER_CONFIG_BYTES]{};
        char requestedRpmKey[16]{};
        char moduleName[16]{};
        float requestedRpm=0;
        PoolDeviceTarget desired, effective;
        PoolDeviceFeedback feedback;
        uint32_t revision=0;
        PoolInterlockState interlockState=PoolInterlockState::Ready;
        struct { bool cancel(ActuatorOverrideReason) { return true; } } overrideTimer;
    } slots_[POOL_DEVICE_MAX];
    ConfigVariable<float,0> cfgRequestedRpmVar_[POOL_DEVICE_MAX]{};
    ConfigVariable<char,0> cfgDriverVar_[POOL_DEVICE_MAX]{};
    ConfigStore storage;
    ConfigStore* cfgStore_=&storage;
    const PoolConfigurationService* poolConfigurationSvc_=nullptr;
    bool runtimeReady_=true;
    unsigned ticks=0;
    PoolDeviceModule() {
        for(uint8_t i=0;i<POOL_DEVICE_MAX;++i) {
            auto& s=slots_[i];
            assert(applyPoolSerialProfile(PoolSerialModel::AquaGem,s.driverConfig));
            assert(serializePoolDriverConfig(s.driverConfig,s.driverJson,sizeof(s.driverJson)));
            snprintf(s.requestedRpmKey,sizeof(s.requestedRpmKey),"act%u_rpm",i);
            snprintf(s.moduleName,sizeof(s.moduleName),"pdm/pd%u",i);
            cfgRequestedRpmVar_[i]={s.requestedRpmKey,"requested_rpm",s.moduleName,ConfigType::Float,&s.requestedRpm,ConfigPersistence::Persistent,0};
            cfgDriverVar_[i]={nullptr,"driver",s.moduleName,ConfigType::CharArray,s.driverJson,ConfigPersistence::Persistent,sizeof(s.driverJson)};
            storage.keys[&s.requestedRpm]=s.requestedRpmKey;
        }
        storage.validator=[](void* ctx,const ConfigCandidate& c){return static_cast<PoolDeviceModule*>(ctx)->validateSpeedConfiguration_(c);};
        storage.context=this;
    }
    bool lockState_() const { return true; }
    void unlockState_() const {}
    bool dependenciesSatisfied_(uint8_t, bool) const { return true; }
    bool maxUptimeReached_(const Slot&) const { return false; }
    void tickDevices_(uint32_t, bool) { ++ticks; }
    bool persistRunning_(uint8_t, bool) { return true; } // Run-state persistence has a separate integration suite.
    bool configurationSetpoint_(uint8_t,float&) const;
    void syncConfigurationSetpoint_(uint8_t);
    bool restoreRequestedSpeed_(uint8_t);
    bool validateSpeedConfiguration_(const ConfigCandidate&) const;
    PoolDeviceSvcStatus svcSetRunningImpl_(uint8_t,uint8_t);
    PoolDeviceSvcStatus svcSetManualRunningImpl_(uint8_t,uint8_t);
    PoolDeviceSvcStatus svcSetRunningAtSetpointImpl_(uint8_t,uint8_t,float);
    PoolDeviceSvcStatus svcSetTargetImpl_(uint8_t,const PoolDeviceTarget*);
    PoolDeviceSvcStatus setTarget_(uint8_t,const PoolDeviceTarget*,bool);
    PoolDeviceSvcStatus setRunning_(uint8_t,uint8_t,bool,const float* = nullptr);
    void boot() {
        for(uint8_t i=0;i<POOL_DEVICE_MAX;++i) {
            storage.loadPersistentVar(cfgRequestedRpmVar_[i]);
            assert(restoreRequestedSpeed_(i));
            slots_[i].desired={false,slots_[i].driverConfig.capabilities.startup};
            syncConfigurationSetpoint_(i);
        }
    }
};

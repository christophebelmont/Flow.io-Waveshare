#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "Core/ConfigCandidate.h"
#include "Core/CommandRegistry.h"
#include "Core/ErrorCodes.h"
#include "Core/NvsKeys.h"
#include "Core/Services/IPoolConfiguration.h"
#include "Core/Services/IPoolDevice.h"
#include "Modules/PoolDeviceModule/PoolDeviceModuleDataModel.h"
#include "Core/Services/IActivityLog.h"
#include "Core/AlarmIds.h"
#include "Domain/Pool/PoolDefaults.h"
#include "Domain/Pool/PoolIds.h"
#include "Core/Control/TimedActuatorOverride.h"

#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGD(...) ((void)0)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
using portMUX_TYPE = int;
constexpr int portMUX_INITIALIZER_UNLOCKED = 0;

uint32_t testNowMs = 1000;
uint32_t& clockMs = testNowMs;

// Byte-backed NVS fake with injectable persistence failures and real variable
// definitions. Each call to migrateModes_ below is production code.
struct ConfigStore {
    std::map<std::string, uint8_t> records;
    std::map<const void*, std::string> registered;
    int writesBeforeFailure = -1;
    bool write(const char* key, uint8_t value) {
        if (writesBeforeFailure == 0) return false;
        if (writesBeforeFailure > 0) --writesBeforeFailure;
        records[key] = value;
        return true;
    }
    template<class T, size_t H> void registerVar(ConfigVariable<T,H>& var) { registered[var.value] = var.nvsKey; }
    template<class T, size_t H> void loadPersistentVar(ConfigVariable<T,H>& var) {
        auto it = records.find(var.nvsKey);
        if (it != records.end()) *var.value = T(it->second);
    }
    template<class T, size_t H> bool set(ConfigVariable<T,H>& var, T value) {
        if (!write(var.nvsKey, uint8_t(value))) return false;
        *var.value = value;
        return true;
    }
    bool persistentKeyExists(const char* key, bool& exists) { exists = records.count(key); return true; }
    bool persistRegisteredValue(const void* ptr) { return write(registered.at(ptr).c_str(), *static_cast<const uint8_t*>(ptr)); }
    bool writeRuntimeBlob(const char* key, const void* ptr, size_t size) { assert(size == 1); return write(key, *static_cast<const uint8_t*>(ptr)); }
    bool applyJson(const char* patch) {
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, patch)) return false;
        const std::pair<const char*, uint8_t> values[] = {
            {NvsKeys::PoolLogic::OperatingMode, doc["poollogic/modes"]["operating_mode"].as<uint8_t>()},
            {NvsKeys::PoolLogic::PhCorrectionType, doc["poollogic/ph"]["correction_type"].as<uint8_t>()}};
        for (const auto& value : values) {
            if (!write(value.first, value.second)) return false;
            for (const auto& var : registered) if (var.second == value.first)
                *static_cast<uint8_t*>(const_cast<void*>(var.first)) = value.second;
        }
        return true;
    }
    bool eraseKey(const char* key) { records.erase(key); return true; }
};

void Log::warn(LogModuleId, const char*, ...) {}
using SpiRamJsonDocument = DynamicJsonDocument;
// COMMAND_HELPERS
// CONTROL_CONSTANTS
class PoolLogicModule {
public:
    struct OverrideLock { explicit OverrideLock(int) {} };
    int overrideMutex_ = 0;
    bool cmdOperatingModeSet_(const CommandRequest&, char*, size_t);
    // PRODUCTION_TYPES
    // PRODUCTION_STATE
    // PRODUCTION_CONFIG
    bool robotWasForced_ = false;
    ConfigStore storage;
    ConfigStore* cfgStore_ = &storage;
    bool actual[POOL_DEVICE_MAX]{}, guided[POOL_DEVICE_MAX]{}, forced[POOL_DEVICE_MAX]{};
    bool present[POOL_DEVICE_MAX];
    bool failRelease = false, failTarget = false;
    unsigned releases = 0, targetWrites = 0;
    bool window = false, pressureAlarm = false, tankAlarm = false;
    bool analogValid = true, digitalValid = true, clockSynced = true;
    float ph = 8, orp = 400, water = 25, air = 10, psi = 1;
    bool levelLow = false;
    ActuatorOverridePolicy policies[POOL_DEVICE_MAX]{};
    PoolDeviceService service{};
    const PoolDeviceService* poolSvc_ = &service;
    struct Alarms { void* ctx; bool (*isActive)(void*, AlarmId); } alarms;
    Alarms* alarmSvc_ = &alarms;
    struct Time { void* ctx; bool (*isSynced)(void*); } time;
    Time* timeSvc_ = &time;
    struct Scheduler { void* ctx; bool (*isActive)(void*, uint8_t); } scheduler;
    Scheduler* schedSvc_ = &scheduler;
    PoolLogicModule() : alarms{this, [](void* ctx, AlarmId id) {
        auto& s = *static_cast<PoolLogicModule*>(ctx);
        return id == AlarmId::PoolPsiHigh ? s.pressureAlarm :
            (id == AlarmId::PoolPhTankLow || id == AlarmId::PoolChlorineTankLow) && s.tankAlarm;
    }}, time{this, [](void* ctx) { return static_cast<PoolLogicModule*>(ctx)->clockSynced; }},
        scheduler{this, [](void* ctx, uint8_t) { return static_cast<PoolLogicModule*>(ctx)->window; }} {
        for (auto& value : present) value = true;
        service.ctx = this;
        service.meta = [](void* ctx, uint8_t slot, PoolDeviceSvcMeta* meta) {
            auto& s = *static_cast<PoolLogicModule*>(ctx);
            if (!s.present[slot]) return POOLDEV_SVC_ERR_UNKNOWN_SLOT;
            *meta = PoolDeviceSvcMeta{};
            meta->guidedOn = s.guided[slot];
            meta->control.mode = s.forced[slot] ? ActuatorControlMode::Forced : ActuatorControlMode::Guided;
            return POOLDEV_SVC_OK;
        };
        service.releaseOverride = [](void* ctx, uint8_t slot, ActuatorOverrideReason) {
            auto& s = *static_cast<PoolLogicModule*>(ctx);
            ++s.releases;
            if (s.failRelease) return POOLDEV_SVC_ERR_IO;
            s.forced[slot] = false;
            return POOLDEV_SVC_OK;
        };
        service.setOverridePolicies = [](void* ctx, const ActuatorOverridePolicy* p, uint8_t count) {
            auto& s = *static_cast<PoolLogicModule*>(ctx);
            std::copy(p, p + count, s.policies);
            return POOLDEV_SVC_OK;
        };
        storage.registerVar(operatingModeVar_);
        storage.registerVar(phCorrectionTypeVar_);
        storage.registerVar(disinfectionTypeVar_);
        modesReady_ = bootControlReady_ = true;
        pidSampleMs_ = 100;
    }
    struct DeviceControlInfo { bool available; bool guidedOn; };
    DeviceControlInfo readDeviceControlInfo_(uint8_t slot) const { return {present[slot], guided[slot]}; }
    bool writeDeviceDesired_(uint8_t slot, bool on) {
        ++targetWrites;
        if (failTarget) return false;
        actual[slot] = guided[slot] = on;
        return true;
    }
    void resetOverridePolicies_() { for (auto& p : policies) p.ready = false; }
    bool readDeviceActualOn_(uint8_t slot, bool& on) const { on = actual[slot]; return present[slot]; }
    bool guidedDeviceOn_(uint8_t slot, bool) const { return guided[slot]; }
    bool currentFiltrationWindowActive_(uint8_t, uint8_t, bool& active) const { active = window; return clockSynced; }
    bool loadAnalogSensor_(IoId id, float& value, uint32_t* ts = nullptr) const {
        if (ts) *ts = clockMs;
        if (id == phIoId_) value = ph;
        else if (id == orpIoId_) value = orp;
        else if (id == waterTempIoId_) value = water;
        else if (id == airTempIoId_) value = air;
        else value = psi;
        return analogValid;
    }
    bool loadDigitalSensor_(IoId id, bool& value) const { value = id == levelIoId_ && levelLow; return digitalValid; }
    void applyDeviceControl_(uint8_t slot, const char*, DeviceFsm& fsm, bool on, uint32_t) {
        writeDeviceDesired_(slot, on); fsm.lastDesired = on;
    }
    uint32_t stateUptimeSec_(const DeviceFsm& state, uint32_t now) const { return (now - state.stateSinceMs) / 1000; }
    template<class... T> void emitActivity_(T...) {}
    template<class... T> void emitDeviceActivity_(T...) {}
    bool isDisinfectionType_(DisinfectionType type) const { return disinfectionType_ == type; }
    bool currentO2LocalTime_(uint16_t& day, uint16_t& week, uint8_t& weekday, uint8_t& hour) const {
        day = 100; week = 14; weekday = 0; hour = 23; return clockSynced;
    }
    bool readPoolDeviceFlowLh_(uint8_t, float& flow) const { flow = 2; return true; }
    void setO2ProtocolState_(uint8_t state, uint8_t reason, uint32_t) { o2ProtocolState_ = state; o2BlockReason_ = reason; }
    void persistO2Protocol_(uint32_t, bool) {}
    bool isO2DoseDay_(uint8_t) const { return true; }
    float computeO2WeeklyDoseMl_(bool, float) const { return 1000; }
    bool stepO2Protocol_(bool, uint32_t, bool, float, bool, bool, uint32_t, bool&, bool&);
    void syncModeStateTraces_(uint32_t) {}
    void syncDeviceState_(uint8_t, DeviceFsm&, uint32_t, bool&, bool&);
    void resetTemporalPidState_(TemporalPidState&, uint32_t);
    bool stepTemporalPid_(TemporalPidState&,float,float,float,float,float,int32_t,bool,uint32_t,bool&,uint32_t&);
    void runControlLoop_(uint32_t);
    void prepare(PoolOperatingMode mode, uint8_t treatment = DisinfectionChlorineBromine) {
        operatingMode_ = uint8_t(mode); phCorrectionType_ = uint8_t(PoolPhCorrection::Minus); disinfectionType_ = treatment;
        runControlLoop_(clockMs);
    }
    void tick(uint32_t delta = 1000) { clockMs += delta; runControlLoop_(clockMs); }
};
// PRODUCTION_METHODS

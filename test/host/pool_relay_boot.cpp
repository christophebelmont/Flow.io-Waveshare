#include <cassert>
#include <cmath>
#include <vector>
#include <map>
#include <string>
#include "aquagem_pump_fake.h"
#include "Modules/PoolDeviceModule/Drivers/PoolSerialProfiles.h"
#include "Core/Services/IPoolConfiguration.h"
#include "Core/ConfigTypes.h"
#include <Arduino.h>
#include "Modules/PoolDeviceModule/Drivers/PoolDeviceDriver.h"
#include "Modules/PoolDeviceModule/Drivers/PoolDriverConfig.h"
#include "Modules/PoolDeviceModule/PoolDeviceModuleDataModel.h"
#include "Core/Control/TimedActuatorOverride.h"
#include "Core/Services/PoolDeviceSafety.h"
#include "Core/Services/IPoolDevice.h"
#include "Core/DataKeys.h"

uint32_t testNowMs = 100;
uint64_t esp_timer_get_time() { return uint64_t(testNowMs) * 1000; }
using std::isfinite;
// DEVICE_TYPE
// DEVICE_DEFINITION

template<class... T> void ignore(T&&...) {}
#define LOGI(...) ignore(__VA_ARGS__)
#define LOGW(...) ignore(__VA_ARGS__)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
namespace VolumeUnits { float millilitresToLitres(float ml) { return ml / 1000; } }
namespace SerialEquipmentValues { template<class... T> void publish(T&&...) {} }
namespace InternalAnalogSources { template<class... T> void publishTank(T&&...) {} }
namespace PoolDeviceValues { template<class... T> void publishUptime(T&&...) {} }
constexpr DataKey DATAKEY_POOL_DEVICE_STATE_BASE = DataKeys::PoolDeviceStateBase;
struct RuntimeData {
    PoolDeviceRuntimeData pool;
};
struct DataStore {
    int values = 0;
    RuntimeData runtime{};
    std::vector<DataKey> notifications;
    RuntimeData& dataMutable() { return runtime; }
    void notifyChanged(DataKey key) { notifications.push_back(key); }
};
// RUNTIME_STATE_HELPERS
bool setPoolDeviceRuntimeMetrics(DataStore&, uint8_t, const PoolDeviceRuntimeMetricsEntry&) { return true; }

struct ConfigStore {
    std::map<std::string, std::vector<uint8_t>> blobs;
    bool fail = false;
    unsigned writes = 0;
    bool readRuntimeBlob(const char* key, void* out, size_t size, size_t* length) {
        auto found = blobs.find(key);
        if (fail || found == blobs.end()) return false;
        *length = found->second.size();
        memcpy(out, found->second.data(), std::min(size, *length));
        return true;
    }
    bool writeRuntimeBlob(const char* key, const void* bytes, size_t length) {
        ++writes;
        if (fail) return false;
        const auto* data = static_cast<const uint8_t*>(bytes);
        blobs[key] = {data, data + length};
        return true;
    }
    template<class T, size_t H> bool set(ConfigVariable<T,H>& var, T value) {
        if (fail) return false;
        *var.value = value; return true;
    }
};

struct RelayIo {
    bool on = true, available = true, readable = true;
    bool digital[16]{};
    float analog = 0;
    IoOutputBootPolicy policies[POOL_DEVICE_MAX]{};
    std::vector<bool> writes;
    IOServiceV2 service{};
    RelayIo() {
        service.ctx = this;
        service.outputBootPolicy = [](void* ctx, uint8_t slot, IoOutputBootPolicy* out) {
            *out = static_cast<RelayIo*>(ctx)->policies[slot]; return IO_OK;
        };
        service.runtimeStatus = [](void* ctx, IoId, IoRuntimeStatus* out) {
            out->state = static_cast<RelayIo*>(ctx)->available ? IO_RUNTIME_ACTIVE : IO_RUNTIME_MANUALLY_DISABLED;
            return IO_OK;
        };
        service.readDigital = [](void* ctx, IoId id, uint8_t* value, uint32_t*, IoSeq*) {
            auto& io = *static_cast<RelayIo*>(ctx);
            if (!io.readable) return IO_ERR_HW;
            *value = id == 7 ? io.on : io.digital[id]; return IO_OK;
        };
        service.writeDigital = [](void* ctx, IoId id, uint8_t value, uint32_t, uint8_t owner) {
            assert(owner > 0);
            auto& io = *static_cast<RelayIo*>(ctx);
            io.writes.push_back(value); (id == 7 ? io.on : io.digital[id]) = value; return IO_OK;
        };
        service.readValue = [](void* ctx, IoId, IoValue* out) {
            out->valid = true; out->type = IO_VAL_FLOAT;
            out->v.f = static_cast<RelayIo*>(ctx)->analog; return IO_OK;
        };
        service.writeAnalog = [](void* ctx, IoId, float value, uint32_t, uint8_t) {
            static_cast<RelayIo*>(ctx)->analog = value; return IO_OK;
        };
    }
};

class PoolDeviceModule {
public:
    // DEVICE_SLOT
    PoolDeviceSlot slots_[POOL_DEVICE_MAX]{};
    RelayIo io;
    Pump pump;
    ConfigStore storage;
    ConfigStore* cfgStore_ = &storage;
    const PoolConfigurationService* poolConfigurationSvc_ = nullptr;
    ConfigVariable<float,0> cfgRequestedRpmVar_[POOL_DEVICE_MAX]{};
    const IOServiceV2* ioSvc_ = &io.service;
    DataStore store;
    DataStore* dataStore_ = &store;
    bool runtimeReady_ = false, writesEnabled_ = false, periodReconcilePending_ = false;
    uint8_t resetPendingMask_ = 0;
    static constexpr unsigned RESET_PENDING_DAY = 1, RESET_PENDING_WEEK = 2, RESET_PENDING_MONTH = 4;
    static constexpr uint32_t RUNTIME_PERSIST_INTERVAL_MS = 60000;
    PoolDeviceModule() {
        auto& s = slots_[0];
        s.used = true;
        s.driverConfig.outputs[0] = 7;
        for (uint8_t i = 0; i < POOL_DEVICE_MAX; ++i)
            snprintf(slots_[i].runStateKey, sizeof(slots_[i].runStateKey), "act%u_run", i);
    }
    bool configureDriver_(uint8_t i) {
        auto& s = slots_[i];
        return s.def.enabled && s.driver.configure(s.driverConfig.capabilities.kind).begin(s.driverConfig, ioSvc_, &pump.service, i + 1);
    }
    bool configureRuntime_();
    void commitRuntimeState_(uint8_t, uint32_t, bool refreshCountdown = false);
    bool restoreRunning_(uint8_t);
    bool persistRunning_(uint8_t, bool);
    bool bootDependenciesPending_(uint8_t, uint32_t, bool, bool);
    PoolDeviceSvcStatus setTarget_(uint8_t, const PoolDeviceTarget*, bool);
    bool configurationSetpoint_(uint8_t, float&) const { return false; }
    void saved(bool on, uint8_t slot = 0) { storage.blobs[slots_[slot].runStateKey] = {1, uint8_t(on)}; }
    void advance(uint32_t duration) {
        for (uint32_t elapsed = 0; elapsed < duration; elapsed += 20) {
            testNowMs += 20; tickDevices_(testNowMs);
        }
    }
    void tickDevices_(uint32_t, bool = true);
    PoolDeviceSvcStatus svcSetWritesEnabledImpl_(uint8_t);
    bool dependenciesSatisfied_(uint8_t, bool = false) const;
    static bool maxUptimeReached_(const PoolDeviceSlot&);
    static uint32_t toSeconds_(uint64_t);
    static bool isFiniteNonNegative_(float value) { return isfinite(value) && value >= 0; }
    bool lockState_() const { return true; }
    void unlockState_() const {}
    void syncConfigurationSetpoint_(uint8_t) {}
    void resetDailyCounters_() {}
    void resetWeeklyCounters_() {}
    void resetMonthlyCounters_() {}
    bool reconcilePeriodCountersFromClock_() { return true; }
    void requestPeriodReconcile_() {}
    void persistMetrics_(uint8_t, PoolDeviceSlot&, uint32_t) {}
    const char* blockReasonStr_(uint8_t) { return ""; }
    // These scenarios use guided control; timed override arbitration has its own suite.
    bool resolveOverride_(uint8_t i, bool, uint64_t, bool& refreshCountdown) {
        refreshCountdown = false;
        return slots_[i].desiredOn;
    }
};

// PRODUCTION_FUNCTIONS

void allDriverPolicies()
{
    for (auto kind : {PoolControlKind::Relay, PoolControlKind::Discrete,
                      PoolControlKind::Analog, PoolControlKind::Rs485}) {
        for (bool retain : {false, true}) for (bool initial : {false, true}) for (bool saved : {false, true}) {
            PoolDeviceModule device;
            auto& slot = device.slots_[0];
            auto& config = slot.driverConfig;
            config.capabilities.kind = kind;
            if (kind == PoolControlKind::Rs485) assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, config));
            if (kind == PoolControlKind::Discrete) {
                config.capabilities.stepCount = 2;
                config.capabilities.steps[0] = 50; config.capabilities.steps[1] = 100;
                config.outputs[0] = 8; config.outputs[1] = 9;
            }
            if (kind == PoolControlKind::Analog) config.analogGain = 0.1f;
            device.io.on = false;
            device.io.policies[0] = {initial, retain};
            device.saved(saved);
            device.pump.running = saved;
            assert(device.configureRuntime_());
            const bool expected = retain ? saved : initial;
            assert(slot.desiredOn == expected);
            const auto writes = device.storage.writes;
            device.advance(6000);
            assert(device.io.writes.empty() && device.pump.writes.empty() && device.io.analog == 0);
            assert(device.storage.writes == writes && slot.desiredOn == expected);
            assert(device.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
            device.advance(3000);
            assert(slot.feedback.observedValid && slot.actualOn == expected);
            assert(device.storage.blobs.at("act0_run") == (std::vector<uint8_t>{1, uint8_t(expected)}));
            if (kind == PoolControlKind::Rs485) {
                assert(!device.pump.writes.empty());
                assert(device.pump.writes.front().value == (expected ? 2900 : 1));
            }
            const auto committed = device.storage.writes;
            device.advance(1000);
            assert(device.storage.writes == committed); // No NVS writes per tick.
        }
    }
}

void persistenceAndDriverChange()
{
    PoolDeviceModule original;
    original.io.on = false;
    assert(original.configureRuntime_());
    assert(original.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
    PoolDeviceTarget target{true, 100};
    original.storage.fail = true;
    assert(original.setTarget_(0, &target, true) == POOLDEV_SVC_ERR_IO);
    assert(!original.io.on && !original.slots_[0].desiredOn);
    original.storage.fail = false;
    assert(original.setTarget_(0, &target, true) == POOLDEV_SVC_OK);
    assert(original.io.on);

    // The same logical equipment retains its demand when the physical driver changes.
    PoolDeviceModule reboot;
    reboot.storage.blobs = original.storage.blobs;
    reboot.io.policies[0].retainPrevious = true;
    assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, reboot.slots_[0].driverConfig));
    assert(reboot.configureRuntime_());
    assert(reboot.slots_[0].desiredOn);
    assert(reboot.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
    reboot.advance(3000);
    assert(reboot.pump.running && reboot.pump.writes.front().value == 2900);

    // Stop is applied despite failed persistence, then retried at a bounded cadence.
    original.storage.fail = true;
    target.running = false;
    assert(original.setTarget_(0, &target, true) == POOLDEV_SVC_ERR_IO);
    assert(!original.io.on && !original.slots_[0].desiredOn);
    const auto attempts = original.storage.writes;
    original.advance(4000);
    assert(original.storage.writes == attempts);
    original.storage.fail = false;
    original.advance(1020);
    assert(original.storage.blobs.at("act0_run") == (std::vector<uint8_t>{1, 0}));
    PoolDeviceModule stopped;
    stopped.storage.blobs = original.storage.blobs;
    stopped.io.policies[0] = {true, true};
    assert(stopped.configureRuntime_() && !stopped.slots_[0].desiredOn);

    // Missing, corrupt and future-version records fall back to the existing initial-state field.
    for (auto record : {std::vector<uint8_t>{}, {1}, {2, 1}, {1, 2}, {1, 1, 0}}) {
        for (bool initial : {false, true}) {
            PoolDeviceModule invalid;
            invalid.storage.blobs["act0_run"] = record;
            invalid.io.policies[0] = {initial, true};
            assert(invalid.configureRuntime_());
            assert(invalid.slots_[0].desiredOn == initial);
        }
    }
    PoolDeviceModule missingPolicy;
    missingPolicy.io.service.outputBootPolicy = nullptr;
    assert(missingPolicy.configureRuntime_() && !missingPolicy.slots_[0].desiredOn);
}

void bootFailures()
{
    // An initial ON cannot run until its baseline has been durably saved.
    PoolDeviceModule storageFailure;
    storageFailure.io.on = false;
    storageFailure.io.policies[0].initialOn = true;
    storageFailure.storage.fail = true;
    assert(storageFailure.configureRuntime_());
    assert(storageFailure.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
    assert(!storageFailure.io.on && storageFailure.slots_[0].runStateSaveFailed);
    storageFailure.storage.fail = false;
    storageFailure.advance(5020);
    assert(storageFailure.io.on && !storageFailure.slots_[0].runStateSaveFailed);

    // A frozen serial bus is not a communication outage. Once enabled, an
    // offline pump still triggers the normal safety stop, including across rollover.
    testNowMs = UINT32_MAX - 7000;
    PoolDeviceModule offline;
    assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, offline.slots_[0].driverConfig));
    offline.io.policies[0].retainPrevious = true;
    offline.saved(true);
    offline.pump.offline = true;
    assert(offline.configureRuntime_());
    offline.advance(6000);
    assert(offline.slots_[0].desiredOn && !offline.slots_[0].feedback.communicationLost);
    assert(offline.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
    offline.advance(6000);
    assert(offline.slots_[0].feedback.communicationLost && !offline.slots_[0].desiredOn);
    assert(offline.storage.blobs.at("act0_run") == (std::vector<uint8_t>{1, 0}));
    offline.pump.offline = false;
    offline.advance(6000);
    assert(!offline.pump.running && !offline.slots_[0].desiredOn);
}

void dependencyStartup()
{
    PoolDeviceModule device;
    auto& child = device.slots_[0];
    auto& parent = device.slots_[1];
    child.def.dependsOnMask = 2;
    assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, child.driverConfig));
    parent.used = true;
    parent.driverConfig.outputs[0] = 8;
    device.io.policies[0].retainPrevious = true;
    device.io.policies[1].retainPrevious = true;
    device.saved(true, 0); device.saved(true, 1);
    assert(device.configureRuntime_());
    device.advance(6000);
    assert(child.desiredOn && child.bootResumePending);
    assert(device.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
    device.advance(3000);
    assert(child.actualOn && parent.actualOn && !child.bootResumePending);
    PoolDeviceTarget parentTarget{false, 100};
    assert(device.setTarget_(1, &parentTarget, true) == POOLDEV_SVC_OK);
    device.advance(3000);
    assert(!child.desiredOn && !device.pump.running);
    assert(device.storage.blobs.at("act0_run") == (std::vector<uint8_t>{1, 0}));
    parentTarget.running = true;
    assert(device.setTarget_(1, &parentTarget, true) == POOLDEV_SVC_OK);
    device.advance(3000);
    assert(!child.desiredOn && !device.pump.running); // No automatic rearming after safety stop.
}

static void countdownNotifications()
{
    DataStore store;
    PoolDeviceRuntimeStateEntry state{};
    state.valid = true;
    state.control.mode = ActuatorControlMode::Forced;
    state.control.remainingSec = 25;
    state.tsMs = 100;
    assert(setPoolDeviceRuntimeState(store, 0, state, false));
    store.notifications.clear();

    auto& stored = store.runtime.pool.state[0];
    for (uint32_t seconds = 1; seconds < 10; ++seconds) {
        state.control.remainingSec = 25 - seconds;
        state.tsMs += 1000;
        assert(!setPoolDeviceRuntimeState(store, 0, state, false));
        assert(stored.control.remainingSec == 25 - seconds);
        assert(stored.sequence == 1);
    }
    assert(store.notifications.empty());

    state.control.remainingSec = 15;
    state.tsMs += 1000;
    assert(setPoolDeviceRuntimeState(store, 0, state, true));
    assert(stored.sequence == 2);
    assert(store.notifications == std::vector<DataKey>{DATAKEY_POOL_DEVICE_STATE_BASE});
    store.notifications.clear();

    state.control.mode = ActuatorControlMode::Guided;
    state.control.remainingSec = 0;
    assert(setPoolDeviceRuntimeState(store, 0, state, false));
    assert(store.notifications == std::vector<DataKey>{DATAKEY_POOL_DEVICE_STATE_BASE});
}

int main()
{
    countdownNotifications();
    allDriverPolicies();
    persistenceAndDriverChange();
    bootFailures();
    dependencyStartup();
    // An ON latch survives initialization, the frozen boot phase, first write
    // enable, and the scheduler's matching ON demand without a single OFF write.
    PoolDeviceModule retained;
    retained.io.policies[0].retainPrevious = true;
    retained.saved(true);
    assert(retained.configureRuntime_());
    assert(retained.slots_[0].actualOn && retained.slots_[0].desiredOn);
    auto& stored = retained.store.runtime.pool.state[0];
    assert(stored.actualOn && retained.io.writes.empty());
    for (testNowMs = 120; testNowMs <= 1000; testNowMs += 20) retained.tickDevices_(testNowMs);
    assert(retained.slots_[0].actualOn && retained.io.writes.empty());
    assert(retained.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
    retained.slots_[0].desired.running = retained.slots_[0].desiredOn = true;
    retained.tickDevices_(testNowMs += 200);
    assert(retained.io.on && retained.io.writes.empty());
    const auto stable = stored;
    retained.store.notifications.clear();
    retained.advance(1000);
    assert(retained.store.notifications.empty());
    assert(stored.feedback.observedAtMs > stable.feedback.observedAtMs);
    assert(stored.tsMs == stable.tsMs && stored.sequence == stable.sequence);
    // End of the filtration window (or a controller safety stop) still stops.
    retained.slots_[0].desired.running = retained.slots_[0].desiredOn = false;
    retained.tickDevices_(testNowMs += 200);
    assert(!retained.io.on && retained.io.writes == std::vector<bool>{false});
    assert(retained.store.notifications == std::vector<DataKey>{DATAKEY_POOL_DEVICE_STATE_BASE});
    assert(stored.sequence == stable.sequence + 1 && stored.tsMs == testNowMs);

    // Cold boot / initial OFF is not promoted to ON until requested.
    PoolDeviceModule stopped;
    stopped.io.on = false;
    assert(stopped.configureRuntime_());
    stopped.tickDevices_(testNowMs);
    assert(stopped.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
    assert(!stopped.slots_[0].actualOn && stopped.io.writes.empty());
    stopped.slots_[0].desired.running = stopped.slots_[0].desiredOn = true;
    stopped.tickDevices_(testNowMs += 200);
    assert(stopped.io.on && stopped.io.writes == std::vector<bool>{true});

    for (bool dependencyFailure : {false, true}) {
        PoolDeviceModule unsafe;
        unsafe.io.policies[0].initialOn = true;
        if (dependencyFailure) unsafe.slots_[0].def.dependsOnMask = 2; // Unavailable parent.
        else {
            unsafe.slots_[0].def.maxUptimeDaySec = 10;
            unsafe.slots_[0].runningMsDay = 10000;
        }
        assert(unsafe.configureRuntime_());
        unsafe.tickDevices_(testNowMs);
        assert(unsafe.io.on && unsafe.io.writes.empty());
        assert(unsafe.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
        unsafe.advance(20100);
        assert(!unsafe.slots_[0].desiredOn);
        assert(!unsafe.io.on && unsafe.io.writes == std::vector<bool>{false});
    }

    PoolDeviceModule overrideBaseline;
    overrideBaseline.slots_[0].overrideRestored = true;
    overrideBaseline.slots_[0].overrideBaseline = false;
    assert(overrideBaseline.configureRuntime_());
    assert(overrideBaseline.slots_[0].actualOn && !overrideBaseline.slots_[0].desiredOn);

    PoolDeviceModule unreadable;
    unreadable.io.readable = false;
    assert(unreadable.configureRuntime_());
    assert(!unreadable.slots_[0].feedback.observedValid && !unreadable.slots_[0].desiredOn);
    assert(unreadable.io.writes.empty());
    testNowMs += 1001;
    assert(unreadable.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
    assert(!unreadable.io.on && unreadable.io.writes == std::vector<bool>{false});

    PoolDeviceModule disabledOutput;
    disabledOutput.io.available = false;
    assert(disabledOutput.configureRuntime_());
    assert(!disabledOutput.slots_[0].feedback.observedValid && !disabledOutput.slots_[0].desiredOn);
    assert(disabledOutput.io.writes.empty());
}

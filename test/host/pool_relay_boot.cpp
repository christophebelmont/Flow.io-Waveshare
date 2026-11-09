#include <cassert>
#include <cmath>
#include <vector>
#include <Arduino.h>
#include "Modules/PoolDeviceModule/Drivers/PoolDeviceDriver.h"
#include "Modules/PoolDeviceModule/Drivers/PoolDriverConfig.h"
#include "Modules/PoolDeviceModule/PoolDeviceModuleDataModel.h"
#include "Core/Control/TimedActuatorOverride.h"
#include "Core/Services/PoolDeviceSafety.h"
#include "Core/Services/IPoolDevice.h"

uint32_t testNowMs = 100;
uint64_t esp_timer_get_time() { return uint64_t(testNowMs) * 1000; }
using std::isfinite;
// DEVICE_TYPE
// DEVICE_DEFINITION

template<class... T> void ignore(T&&...) {}
#define LOGI(...) ignore(__VA_ARGS__)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
namespace DataKeys { constexpr unsigned PoolDeviceOverrides = 0; }
namespace VolumeUnits { float millilitresToLitres(float ml) { return ml / 1000; } }
namespace SerialEquipmentValues { template<class... T> void publish(T&&...) {} }
namespace InternalAnalogSources { template<class... T> void publishTank(T&&...) {} }
namespace PoolDeviceValues { template<class... T> void publishUptime(T&&...) {} }
struct Store {
    int values = 0;
    PoolDeviceRuntimeStateEntry states[POOL_DEVICE_MAX]{};
    void notifyChanged(unsigned) {}
};
bool setPoolDeviceRuntimeState(Store& store, uint8_t i, const PoolDeviceRuntimeStateEntry& state) {
    store.states[i] = state; return true;
}
bool poolDeviceRuntimeState(Store& store, uint8_t i, PoolDeviceRuntimeStateEntry& state) {
    state = store.states[i]; return true;
}
bool setPoolDeviceRuntimeMetrics(Store&, uint8_t, const PoolDeviceRuntimeMetricsEntry&) { return true; }

struct RelayIo {
    bool on = true, available = true, readable = true;
    std::vector<bool> writes;
    IOServiceV2 service{};
    RelayIo() {
        service.ctx = this;
        service.runtimeStatus = [](void* ctx, IoId id, IoRuntimeStatus* out) {
            assert(id == 7); // Binding is independent of the equipment slot.
            out->state = static_cast<RelayIo*>(ctx)->available ? IO_RUNTIME_ACTIVE : IO_RUNTIME_MANUALLY_DISABLED;
            return IO_OK;
        };
        service.readDigital = [](void* ctx, IoId, uint8_t* value, uint32_t*, IoSeq*) {
            auto& io = *static_cast<RelayIo*>(ctx);
            if (!io.readable) return IO_ERR_HW;
            *value = io.on; return IO_OK;
        };
        service.writeDigital = [](void* ctx, IoId, uint8_t value, uint32_t, uint8_t owner) {
            assert(owner == 1);
            auto& io = *static_cast<RelayIo*>(ctx);
            io.writes.push_back(value); io.on = value; return IO_OK;
        };
        service.readValue = [](void*, IoId, IoValue*) { return IO_ERR_TYPE_MISMATCH; };
        service.writeAnalog = [](void*, IoId, float, uint32_t, uint8_t) { return IO_ERR_TYPE_MISMATCH; };
    }
};

class PoolDeviceModule {
public:
    // DEVICE_SLOT
    PoolDeviceSlot slots_[POOL_DEVICE_MAX]{};
    RelayIo io;
    const IOServiceV2* ioSvc_ = &io.service;
    Store store;
    Store* dataStore_ = &store;
    bool runtimeReady_ = false, writesEnabled_ = false, periodReconcilePending_ = false;
    uint8_t resetPendingMask_ = 0;
    static constexpr unsigned RESET_PENDING_DAY = 1, RESET_PENDING_WEEK = 2, RESET_PENDING_MONTH = 4;
    static constexpr uint32_t RUNTIME_PERSIST_INTERVAL_MS = 60000;
    PoolDeviceModule() {
        auto& s = slots_[0];
        s.used = true;
        s.driverConfig.outputs[0] = 7;
    }
    bool configureDriver_(uint8_t i) {
        auto& s = slots_[i];
        return s.def.enabled && s.driver.configure(s.driverConfig.capabilities.kind).begin(s.driverConfig, ioSvc_, nullptr, i + 1);
    }
    bool configureRuntime_();
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
    bool resolveOverride_(uint8_t i, bool, uint64_t) { return slots_[i].desiredOn; }
};

// PRODUCTION_FUNCTIONS

int main()
{
    // An ON latch survives initialization, the frozen boot phase, first write
    // enable, and the scheduler's matching ON demand without a single OFF write.
    PoolDeviceModule retained;
    assert(retained.configureRuntime_());
    assert(retained.slots_[0].actualOn && retained.slots_[0].desiredOn);
    assert(retained.store.states[0].actualOn && retained.io.writes.empty());
    for (testNowMs = 120; testNowMs <= 1000; testNowMs += 20) retained.tickDevices_(testNowMs);
    assert(retained.slots_[0].actualOn && retained.io.writes.empty());
    assert(retained.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
    retained.slots_[0].desired.running = retained.slots_[0].desiredOn = true;
    retained.tickDevices_(testNowMs += 200);
    assert(retained.io.on && retained.io.writes.empty());
    // End of the filtration window (or a controller safety stop) still stops.
    retained.slots_[0].desired.running = retained.slots_[0].desiredOn = false;
    retained.tickDevices_(testNowMs += 200);
    assert(!retained.io.on && retained.io.writes == std::vector<bool>{false});

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
        if (dependencyFailure) unsafe.slots_[0].def.dependsOnMask = 2; // Unavailable parent.
        else {
            unsafe.slots_[0].def.maxUptimeDaySec = 10;
            unsafe.slots_[0].runningMsDay = 10000;
        }
        assert(unsafe.configureRuntime_());
        unsafe.tickDevices_(testNowMs);
        assert(unsafe.io.on && unsafe.io.writes.empty());
        assert(unsafe.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
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

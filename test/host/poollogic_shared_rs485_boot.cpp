#include <cassert>
#include <cstdint>

// Small host fake: the decision function itself comes from
// PoolLogicControl.cpp via the Python test, not a rewritten copy.
enum class ActivityReason : uint8_t { Auto };
class PoolLogicModule {
public:
    // DEVICE_FSM
    struct DeviceControlInfo {
        bool available = true;
        bool sharedRs485 = true;
        bool serialStartupPending = true;
        bool guidedOn = true;
        bool forced = false;
        uint8_t blockReason = 0;
    };
    DeviceControlInfo info{};
    bool accept = false;
    unsigned attempts = 0;
    unsigned activities = 0;
    bool lastSent = false;
    DeviceControlInfo readDeviceControlInfo_(uint8_t) const { return info; }
    bool writeDeviceDesired_(uint8_t, bool on) {
        lastSent = on;
        ++attempts;
        return accept;
    }
    void emitDeviceActivity_(bool, bool, uint8_t, const char*, ActivityReason) {
        ++activities;
    }
    void applyDeviceControl_(uint8_t, const char*, DeviceFsm&, bool, uint32_t);
};
#define LOGI(...) ((void)0)
// PRODUCTION_METHOD

int main() {
    PoolLogicModule logic;
    PoolLogicModule::DeviceFsm fsm{};

    // Boot ON is blocked while the filtration dependency starts.
    logic.applyDeviceControl_(7, "Polytropic", fsm, true, 100);
    assert(logic.attempts == 1 && logic.lastSent);
    assert(fsm.rejectedSubmissionPending);
    logic.applyDeviceControl_(7, "Polytropic", fsm, true, 4000);
    assert(logic.attempts == 1);

    // Retry rejected command only after the 5-second backoff.
    logic.accept = true;
    logic.applyDeviceControl_(7, "Polytropic", fsm, true, 5100);
    assert(logic.attempts == 2 && !fsm.rejectedSubmissionPending);

    // An accepted call during dependency wait is not yet a confirmed
    // physical ON. Reassert startup demand, but only until readback.
    fsm.known = true;
    fsm.on = false;
    logic.applyDeviceControl_(7, "Polytropic", fsm, true, 10100);
    assert(logic.attempts == 3 && logic.lastSent);
    logic.info.serialStartupPending = false;
    fsm.on = true;
    logic.applyDeviceControl_(7, "Polytropic", fsm, true, 15100);
    assert(logic.attempts == 3);

    // The panel stops the device after startup: no stale Flow.io retry.
    fsm.on = false;
    logic.applyDeviceControl_(7, "Polytropic", fsm, true, 40100);
    assert(logic.attempts == 3);

    // A genuine new Flow.io demand is still forwarded.
    logic.applyDeviceControl_(7, "Polytropic", fsm, false, 40200);
    assert(logic.attempts == 4 && !logic.lastSent);

    // Safety OFF can be retried if physical ON remains and interlock trips.
    fsm.on = true;
    logic.info.blockReason = 1;
    logic.applyDeviceControl_(7, "Polytropic", fsm, false, 45300);
    assert(logic.attempts == 5 && !logic.lastSent);

    // Another device may be retried after a service rejection even though
    // its initial startup is long complete.
    logic.info.blockReason = 0;
    logic.accept = false;
    logic.applyDeviceControl_(7, "Polytropic", fsm, true, 45500);
    assert(logic.attempts == 6 && fsm.rejectedSubmissionPending);
    logic.applyDeviceControl_(7, "Polytropic", fsm, true, 46500);
    assert(logic.attempts == 6);
    logic.accept = true;
    logic.applyDeviceControl_(7, "Polytropic", fsm, true, 50500);
    assert(logic.attempts == 7 && !fsm.rejectedSubmissionPending);
    logic.applyDeviceControl_(7, "Polytropic", fsm, true, 60500);
    assert(logic.attempts == 7);
}

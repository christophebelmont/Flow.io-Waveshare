#include "Core/Services/PoolInterlockState.h"
#include "Core/Services/PoolDeviceSafety.h"
#include <cassert>
#include <initializer_list>

int main()
{
    PoolDriverConfig heater;
    heater.dependencyMinimum = 1500;
    PoolDeviceFeedback pump{};
    pump.hasObservation = true;
    pump.observed = {true, 2000};
    pump.observedAtMs = 100;
    pump.communicationDegraded = true;
    pump.error = MODBUS_RESULT_TIMEOUT;
    assert(!poolFeedbackInhibitsRun(PoolControlKind::Rs485, pump));
    assert(!poolDependencyPermitsRun(heater, pump, true, true, 101));
    heater.verifiedFlowGraceMs = 5000;
    assert(!poolDependencyPermitsRun(heater, pump, true, false, 101));
    assert(poolDependencyPermitsRun(heater, pump, true, true, 5100));
    assert(!poolDependencyPermitsRun(heater, pump, true, true, 5101));
    assert(!poolDependencyPermitsRun(heater, pump, false, true, 101));
    pump.communicationDegraded = false;
    assert(poolFeedbackInhibitsRun(PoolControlKind::Rs485, pump));
    const uint32_t now = 6000;
    PoolDeviceFeedback heat{};
    assert(!poolLocalFlowPermitsHeat(heater, heat, false, now));
    heat.telemetry.validBlocks = 1;
    heat.telemetry.raw[0] = 8;
    heat.telemetry.readAtMs[0] = now;
    assert(poolLocalFlowPermitsHeat(heater, heat, false, now));
    heat.telemetry.raw[1] = 0x4000;
    assert(!poolLocalFlowPermitsHeat(heater, heat, true, now));
    using State = PoolInterlockState;
    auto state = updatePoolInterlockState(State::Ready, false, false);
    assert(state == State::Unavailable);
    assert(!poolInterlockBlocked(state));
    // Arbitrarily many idle ticks must never create a fault.
    for (int i = 0; i < 100; ++i) {
        state = updatePoolInterlockState(state, false, false);
        assert(!poolInterlockBlocked(state));
    }
    // A refused command stays diagnosable after the request has been discarded.
    state = updatePoolInterlockState(State::StartRejected, false, false);
    assert(state == State::StartRejected && poolInterlockBlocked(state));
    // Losing a dependency while starting/running stops the device and retains
    // that diagnostic on subsequent ticks, when no request remains.
    state = updatePoolInterlockState(State::Ready, false, true);
    assert(state == State::SafetyStopped);
    state = updatePoolInterlockState(state, false, false);
    assert(state == State::SafetyStopped && poolInterlockBlocked(state));
    // Recovery clears both kinds of diagnostic. It does not create a target.
    for (auto previous : {State::StartRejected, State::SafetyStopped}) {
        assert(updatePoolInterlockState(previous, true, false) == State::Ready);
    }
    // An accepted stop clears the diagnostic to normal unavailability.
    assert(updatePoolInterlockState(State::Ready, false, false) == State::Unavailable);
}

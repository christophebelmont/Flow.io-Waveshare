#include "Core/Services/PoolInterlockState.h"
#include <cassert>
#include <initializer_list>

int main()
{
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

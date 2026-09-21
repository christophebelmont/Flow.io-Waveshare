#pragma once

#include <stdint.h>

// Availability is distinct from a refused start or a safety stop. No state here
// represents a pending request: restoring a dependency never restarts a device.
enum class PoolInterlockState : uint8_t {
    Ready = 0,
    Unavailable = 1,
    StartRejected = 2,
    SafetyStopped = 3
};

inline PoolInterlockState updatePoolInterlockState(PoolInterlockState previous,
                                                  bool dependenciesReady,
                                                  bool running)
{
    if (dependenciesReady) return PoolInterlockState::Ready;
    if (running) return PoolInterlockState::SafetyStopped;
    if (previous == PoolInterlockState::StartRejected ||
        previous == PoolInterlockState::SafetyStopped) return previous;
    return PoolInterlockState::Unavailable;
}

inline bool poolInterlockBlocked(PoolInterlockState state)
{
    return state == PoolInterlockState::StartRejected || state == PoolInterlockState::SafetyStopped;
}

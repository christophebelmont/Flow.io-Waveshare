#pragma once

#include <stdint.h>

// Wire flags shared by the runtime SSE stream and the dashboard controller.
namespace RuntimeEventDomains {
constexpr uint8_t Mode = 1;
constexpr uint8_t Equipment = 2;
constexpr uint8_t Alarm = 4;
constexpr uint8_t Sensors = 8;
constexpr uint8_t All = Mode | Equipment | Alarm | Sensors;
}

struct RuntimeEventBatch {
    uint32_t revision;
    uint8_t domains;
};

/** Coalesce notifications; the owner must synchronize access across tasks. */
class RuntimeEventState {
public:
    void mark(uint8_t domains) { pending_ |= domains & RuntimeEventDomains::All; }
    uint32_t revision() const { return revision_; }

    bool take(uint32_t nowMs, RuntimeEventBatch& batch)
    {
        if (!pending_ || (uint32_t)(nowMs - lastBatchMs_) < 100U) return false;
        if (++revision_ == 0U) revision_ = 1U;
        batch = {revision_, pending_};
        pending_ = 0;
        lastBatchMs_ = nowMs;
        return true;
    }

private:
    uint8_t pending_ = 0;
    uint32_t revision_ = 1;
    uint32_t lastBatchMs_ = 0;
};

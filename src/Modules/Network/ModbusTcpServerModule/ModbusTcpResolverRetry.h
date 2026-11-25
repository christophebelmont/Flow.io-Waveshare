#pragma once
/** Bounded, nonblocking per-client retry policy for a temporarily locked
 * PoolDevice serial-profile snapshot. No UART, socket or FreeRTOS dependencies.
 */
#include <stdint.h>

namespace ModbusTcpServer {
enum class ResolveStatus : uint8_t { Ready, Busy, Invalid };
enum class ResolveDecision : uint8_t { Ready, Retry, Invalid, BusyTimeout };

class ResolverRetryPolicy {
public:
    static constexpr uint32_t kRetryIntervalMs = 25U;
    static constexpr uint32_t kBusyDeadlineMs = 750U;
    void begin(uint32_t nowMs) {
        startedAtMs_ = nowMs;
        lastTriedAtMs_ = nowMs;
        pending_ = false;
    }
    bool pending() const { return pending_; }
    bool due(uint32_t nowMs) const {
        return !pending_ || uint32_t(nowMs - lastTriedAtMs_) >= kRetryIntervalMs ||
            uint32_t(nowMs - startedAtMs_) >= kBusyDeadlineMs;
    }
    ResolveDecision consider(ResolveStatus result, uint32_t nowMs) {
        if (result == ResolveStatus::Ready) {
            reset();
            return ResolveDecision::Ready;
        }
        if (result == ResolveStatus::Invalid) {
            reset();
            return ResolveDecision::Invalid;
        }
        if (uint32_t(nowMs - startedAtMs_) >= kBusyDeadlineMs) {
            reset();
            return ResolveDecision::BusyTimeout;
        }
        pending_ = true;
        lastTriedAtMs_ = nowMs;
        return ResolveDecision::Retry;
    }
    void reset() {
        pending_ = false;
        startedAtMs_ = 0U;
        lastTriedAtMs_ = 0U;
    }
private:
    uint32_t startedAtMs_ = 0U;
    uint32_t lastTriedAtMs_ = 0U;
    bool pending_ = false;
};
} // namespace ModbusTcpServer

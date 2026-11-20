#pragma once
/**
 * @brief Fixed-capacity TCP session admission and stream framing.
 *
 * Three ports, two slots each. No heap allocations. This layer does not
 * own sockets or touch UART: the network task retains all socket operations.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ModbusTcpAdu.h"
#include "ModbusTcpServerPolicy.h"

namespace ModbusTcpServer {
class TcpSessions {
public:
    enum class FeedResult : uint8_t { Incomplete, Complete, Invalid, Busy };
    struct Session {
        bool occupied = false;
        bool requestReady = false;
        uint16_t bytesUsed = 0U;
        uint8_t frame[ModbusTcpAdu::kMaxAdu]{};
    };

    /** Returns slot index 0..5, or -1 if this port is at capacity. */
    int8_t accept(uint8_t portIndex) {
        if (!validPortIndex(portIndex)) return -1;
        const uint8_t first = portIndex * kClientsPerPort;
        for (uint8_t i = first; i < first + kClientsPerPort; ++i) {
            if (sessions_[i].occupied) continue;
            sessions_[i] = {};
            sessions_[i].occupied = true;
            return int8_t(i);
        }
        return -1;
    }

    void release(uint8_t slot) {
        if (slot < kMaxClients) sessions_[slot] = {};
    }

    /** Consume exactly one ADU; callers may pass TCP fragments or coalesced
     * packets. consumed tells the caller where to resume in the input buffer.
     * A completed frame must be dispatched and clearRequest() called before
     * feeding the next frame on the same connection.
     */
    FeedResult feed(uint8_t slot, const uint8_t* input, size_t size,
                    size_t& consumed) {
        consumed = 0U;
        if (slot >= kMaxClients || !sessions_[slot].occupied ||
            (!input && size)) return FeedResult::Invalid;
        Session& s = sessions_[slot];
        if (s.requestReady) return FeedResult::Busy;
        while (consumed < size) {
            if (s.bytesUsed >= sizeof(s.frame)) {
                s.bytesUsed = 0U;
                return FeedResult::Invalid;
            }
            s.frame[s.bytesUsed++] = input[consumed++];
            if (s.bytesUsed < ModbusTcpAdu::kMbapPrefix) continue;
            const size_t expected = ModbusTcpAdu::expectedSize(s.frame, s.bytesUsed);
            if (!expected || expected > sizeof(s.frame)) {
                s.bytesUsed = 0U;
                return FeedResult::Invalid;
            }
            if (s.bytesUsed == expected) {
                s.requestReady = true;
                return FeedResult::Complete;
            }
        }
        return FeedResult::Incomplete;
    }

    /** Exact bytes needed to finish the current MBAP header or TCP ADU.
     * Readers must not consume bytes beyond this boundary: a persistent
     * connection may pipeline another ADU, which must remain in the socket
     * until the previous Modbus response has been sent.
     */
    size_t bytesNeeded(uint8_t slot) const {
        if (slot >= kMaxClients) return 0U;
        const Session& s = sessions_[slot];
        if (!s.occupied || s.requestReady) return 0U;
        if (s.bytesUsed < ModbusTcpAdu::kMbapPrefix)
            return ModbusTcpAdu::kMbapPrefix - s.bytesUsed;
        const size_t expected = ModbusTcpAdu::expectedSize(s.frame, s.bytesUsed);
        return expected > s.bytesUsed && expected <= sizeof(s.frame)
            ? expected - s.bytesUsed : 0U;
    }

    /** Read transaction ID, unit and function even for an invalid/unsupported PDU. */
    bool headerRequest(uint8_t slot, ModbusTcpAdu::Request& out) const {
        if (slot >= kMaxClients) return false;
        const Session& s = sessions_[slot];
        if (!s.occupied || !s.requestReady || s.bytesUsed < 8U ||
            ModbusTcpAdu::expectedSize(s.frame, s.bytesUsed) != s.bytesUsed)
            return false;
        out = ModbusTcpAdu::Request{};
        out.transactionId = ModbusTcpAdu::word(s.frame);
        out.unitId = s.frame[6];
        out.function = s.frame[7];
        return true;
    }

    bool request(uint8_t slot, ModbusTcpAdu::Request& out) const {
        if (slot >= kMaxClients) return false;
        const Session& s = sessions_[slot];
        return s.occupied && s.requestReady &&
            ModbusTcpAdu::decode(s.frame, s.bytesUsed, out);
    }

    void clearRequest(uint8_t slot) {
        if (slot >= kMaxClients || !sessions_[slot].occupied) return;
        sessions_[slot].bytesUsed = 0U;
        sessions_[slot].requestReady = false;
    }

    /** Occupancy is independent of the current socket connectivity state. */
    bool occupied(uint8_t slot) const {
        return slot < kMaxClients && sessions_[slot].occupied;
    }

    /** True only while a TCP request is fragmented/incomplete. An idle,
     * persistent connection without buffered bytes is not timed out.
     */
    bool hasPartialRequest(uint8_t slot) const {
        return slot < kMaxClients && sessions_[slot].occupied &&
               !sessions_[slot].requestReady && sessions_[slot].bytesUsed != 0U;
    }

    uint8_t active(uint8_t portIndex) const {
        if (!validPortIndex(portIndex)) return 0U;
        uint8_t count = 0U;
        for (uint8_t i = portIndex * kClientsPerPort;
             i < (portIndex + 1U) * kClientsPerPort; ++i)
            if (sessions_[i].occupied) ++count;
        return count;
    }
private:
    Session sessions_[kMaxClients]{};
};
} // namespace ModbusTcpServer

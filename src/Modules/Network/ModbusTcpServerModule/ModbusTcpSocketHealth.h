#pragma once
/**
 * @brief Nonblocking TCP peer probing, independent of the Arduino socket layer.
 * A connected TCP socket can be idle indefinitely; EAGAIN is not a failure.
 */
#include <errno.h>
#include <stdint.h>

namespace ModbusTcpServer {
enum class SocketPeerHealth : uint8_t { Alive, NotReady, ReadClosed, Error };

// A receive error is different from an orderly FIN or from a transient
// EAGAIN/EINTR result. Keep the original socket error before close()/stop()
// overwrites errno. Everything here is plain C++ and host-testable.
enum class SocketRxErrorSource : uint8_t { None, DataRecv, PeerProbe };

inline const char* socketRxErrorSourceName(SocketRxErrorSource source)
{
    switch (source) {
        case SocketRxErrorSource::DataRecv: return "recv";
        case SocketRxErrorSource::PeerProbe: return "peek";
        default: return "unknown";
    }
}

struct SocketRxErrorDetail {
    uint32_t atMs = 0U;
    uint32_t sessionId = 0U;
    int32_t fd = -1;
    int32_t socketErrno = 0;
    uint8_t slot = 0U;
    SocketRxErrorSource source = SocketRxErrorSource::None;

    void capture(SocketRxErrorSource where, uint8_t clientSlot,
                 uint32_t connection, int descriptor, int errorCode,
                 uint32_t nowMs)
    {
        source = where;
        slot = clientSlot;
        sessionId = connection;
        fd = descriptor;
        socketErrno = errorCode;
        atMs = nowMs;
    }
};

// Monotonically numbered per-boot TCP admissions distinguish reused slot 0/1.
struct SocketSessionSequence {
    uint32_t current = 0U;
    uint32_t next() {
        ++current;
        if (current == 0U) ++current; // Reserve zero for no active session.
        return current;
    }
};

inline SocketPeerHealth classifySocketPeerProbe(int received, int socketError)
{
    if (received > 0) return SocketPeerHealth::Alive;
    if (received == 0) return SocketPeerHealth::ReadClosed; // TCP FIN, not "no data"
    if (socketError == EAGAIN || socketError == EWOULDBLOCK ||
        socketError == EINTR) return SocketPeerHealth::NotReady;
    return SocketPeerHealth::Error;
}

inline bool canReleaseAfterReadClose(bool readClosed, bool bridgeIdle, bool writing)
{
    // An orderly TCP half-close can still be waiting for its Modbus reply.
    return readClosed && bridgeIdle && !writing;
}
} // namespace ModbusTcpServer

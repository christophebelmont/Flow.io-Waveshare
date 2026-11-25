#pragma once
/**
 * @brief Nonblocking TCP peer probing, independent of the Arduino socket layer.
 * A connected TCP socket can be idle indefinitely; EAGAIN is not a failure.
 */
#include <errno.h>
#include <stdint.h>

namespace ModbusTcpServer {
enum class SocketPeerHealth : uint8_t { Alive, NotReady, ReadClosed, Error };

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

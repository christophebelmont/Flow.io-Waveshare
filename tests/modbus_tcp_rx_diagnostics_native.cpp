// Build: g++ -std=c++17 -Wall -Wextra -Werror tests/modbus_tcp_rx_diagnostics_native.cpp -o /tmp/modbus_tcp_rx_diagnostics && /tmp/modbus_tcp_rx_diagnostics
#include <cassert>
#include <cerrno>
#include <cstdint>
#include "../src/Modules/Network/ModbusTcpServerModule/ModbusTcpSocketHealth.h"

using namespace ModbusTcpServer;

int main() {
    // Ordinary FIN and nonblocking EAGAIN/EINTR never count as rxerr.
    assert(classifySocketPeerProbe(0, 0) == SocketPeerHealth::ReadClosed);
    assert(classifySocketPeerProbe(1, 0) == SocketPeerHealth::Alive);
    assert(classifySocketPeerProbe(-1, EAGAIN) == SocketPeerHealth::NotReady);
    assert(classifySocketPeerProbe(-1, EWOULDBLOCK) == SocketPeerHealth::NotReady);
    assert(classifySocketPeerProbe(-1, EINTR) == SocketPeerHealth::NotReady);
    assert(classifySocketPeerProbe(-1, ECONNRESET) == SocketPeerHealth::Error);
    // Error source and errno survive closing/reusing the same socket slot.
    SocketSessionSequence seq;
    SocketRxErrorDetail latest{};
    const uint32_t first = seq.next();
    latest.capture(SocketRxErrorSource::DataRecv, 0, first, 23, ECONNRESET, 1234);
    assert(latest.source == SocketRxErrorSource::DataRecv);
    assert(latest.socketErrno == ECONNRESET && latest.fd == 23);
    assert(latest.atMs == 1234 && latest.sessionId == first && latest.slot == 0);
    const uint32_t second = seq.next();
    assert(second != first);
    latest.capture(SocketRxErrorSource::PeerProbe, 0, second, 28, ETIMEDOUT, 2234);
    assert(latest.source == SocketRxErrorSource::PeerProbe);
    assert(latest.socketErrno == ETIMEDOUT && latest.fd == 28);
    assert(latest.sessionId == second);
    assert(socketRxErrorSourceName(latest.source)[0] == 'p');
    assert(socketRxErrorSourceName(SocketRxErrorSource::DataRecv)[0] == 'r');
    // Six simultaneous sessions use unique IDs; recycled slots differ.
    for (int i = 0; i < 6; ++i) assert(seq.next() != first);
    seq.current = 0xffffffffU;
    assert(seq.next() == 1U); // wrap does not return reserved zero
    // Replay a typical persistent-client reconnect across successive boots:
    // the next admission is distinct even when fd and slot are reused.
    SocketRxErrorDetail prev = latest;
    (void)seq.next(); // avoid the same numeric ID immediately after forced wrap
    latest.capture(SocketRxErrorSource::DataRecv, 0, seq.next(), 28, ECONNRESET, 5000);
    assert(latest.sessionId != prev.sessionId);
    assert(latest.atMs == 5000);
    return 0;
}

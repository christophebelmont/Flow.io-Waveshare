// Regression checks for unchanged FIN/RST, EAGAIN and EINTR behavior.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pedantic
// tests/modbus_tcp_rx_diagnostics_native.cpp -o /tmp/flow_tcp_health && /tmp/flow_tcp_health
#include <cassert>
#include <cerrno>
#include "../src/Modules/Network/ModbusTcpServerModule/ModbusTcpSocketHealth.h"
using namespace ModbusTcpServer;
int main() {
    assert(classifySocketPeerProbe(0, 0) == SocketPeerHealth::ReadClosed);
    assert(classifySocketPeerProbe(1, 0) == SocketPeerHealth::Alive);
    assert(classifySocketPeerProbe(-1, EAGAIN) == SocketPeerHealth::NotReady);
    assert(classifySocketPeerProbe(-1, EWOULDBLOCK) == SocketPeerHealth::NotReady);
    assert(classifySocketPeerProbe(-1, EINTR) == SocketPeerHealth::NotReady);
    assert(classifySocketPeerProbe(-1, ECONNRESET) == SocketPeerHealth::Error);
    assert(classifySocketPeerProbe(-1, ETIMEDOUT) == SocketPeerHealth::Error);
    assert(!canReleaseAfterReadClose(true, false, false));
    assert(!canReleaseAfterReadClose(true, true, true));
    assert(canReleaseAfterReadClose(true, true, false));
    assert(!canReleaseAfterReadClose(false, true, false));
    return 0;
}

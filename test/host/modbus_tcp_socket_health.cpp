#include <cassert>
#include <cerrno>
#include "Modules/Network/ModbusTcpServerModule/ModbusTcpSocketHealth.h"

int main()
{
    using namespace ModbusTcpServer;
    // Never evict a persistent but idle client on EAGAIN.
    assert(classifySocketPeerProbe(-1, EAGAIN) == SocketPeerHealth::NotReady);
    assert(classifySocketPeerProbe(-1, EWOULDBLOCK) == SocketPeerHealth::NotReady);
    assert(classifySocketPeerProbe(-1, EINTR) == SocketPeerHealth::NotReady);
    assert(classifySocketPeerProbe(1, 0) == SocketPeerHealth::Alive);
    // EOF and RST have different errno/read signatures.
    assert(classifySocketPeerProbe(0, 0) == SocketPeerHealth::ReadClosed);
    assert(classifySocketPeerProbe(-1, ECONNRESET) == SocketPeerHealth::Error);
    // Allow a half-closed Modbus client to receive its in-flight answer.
    assert(!canReleaseAfterReadClose(true, false, false));
    assert(!canReleaseAfterReadClose(true, true, true));
    assert(!canReleaseAfterReadClose(false, true, false));
    assert(canReleaseAfterReadClose(true, true, false));
}

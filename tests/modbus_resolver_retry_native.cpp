// Host regression: g++ -std=c++17 -Wall -Wextra -Werror tests/modbus_resolver_retry_native.cpp -o /tmp/modbus_resolver_retry && /tmp/modbus_resolver_retry
#include <cassert>
#include "../src/Modules/Network/ModbusTcpServerModule/ModbusTcpResolverRetry.h"
using namespace ModbusTcpServer;
int main() {
    ResolverRetryPolicy p;
    p.begin(1000);
    assert(p.consider(ResolveStatus::Ready,1000)==ResolveDecision::Ready && !p.pending());
    p.begin(1000);
    assert(p.consider(ResolveStatus::Invalid,1000)==ResolveDecision::Invalid && !p.pending());
    p.begin(1000);
    assert(p.consider(ResolveStatus::Busy,1000)==ResolveDecision::Retry);
    assert(!p.due(1024) && p.due(1025));
    assert(p.consider(ResolveStatus::Busy,1025)==ResolveDecision::Retry);
    assert(p.consider(ResolveStatus::Ready,1050)==ResolveDecision::Ready && !p.pending());
    p.begin(1000);
    assert(p.consider(ResolveStatus::Busy,1000)==ResolveDecision::Retry);
    assert(p.consider(ResolveStatus::Busy,1725)==ResolveDecision::Retry);
    assert(p.consider(ResolveStatus::Busy,1750)==ResolveDecision::BusyTimeout && !p.pending());
    p.begin(1000);
    assert(p.consider(ResolveStatus::Busy,1000)==ResolveDecision::Retry);
    assert(p.consider(ResolveStatus::Invalid,1025)==ResolveDecision::Invalid);
    p.begin(0xfffffff0U);
    assert(p.consider(ResolveStatus::Busy,0xfffffff0U)==ResolveDecision::Retry);
    assert(!p.due(0x00000005U) && p.due(0x0000000aU));
    assert(p.consider(ResolveStatus::Ready,0x0000000aU)==ResolveDecision::Ready);
    ResolverRetryPolicy clients[6]{};
    clients[0].begin(100); clients[5].begin(100);
    assert(clients[0].consider(ResolveStatus::Busy,100)==ResolveDecision::Retry);
    assert(clients[5].consider(ResolveStatus::Ready,100)==ResolveDecision::Ready);
    assert(clients[0].pending() && !clients[5].pending());
    clients[0].reset(); assert(!clients[0].pending());
    p.begin(1234); assert(p.consider(ResolveStatus::Busy,1234)==ResolveDecision::Retry);
    p.reset(); p.begin(4567);
    assert(p.consider(ResolveStatus::Ready,4567)==ResolveDecision::Ready);
    return 0;
}

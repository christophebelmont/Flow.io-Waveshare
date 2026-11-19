#include <assert.h>
#include "Modules/Network/ModbusTcpServerModule/ModbusTcpLineResolver.h"

namespace {
bool lookup(void* context, uint16_t deviceId, SerialLineProfile& out,
            uint8_t& targetUnitId) {
    ++*static_cast<unsigned*>(context);
    if (deviceId != 7U && deviceId != 0U) return false;
    out.baud = 19200U;
    out.parity = 1U;
    out.stopBits = 2U;
    targetUnitId = 11U;
    return true;
}
}
int main() {
    using namespace ModbusTcpServer;
    LineResolver resolver;
    SerialLineProfile line{};
    uint8_t targetUnitId = 0U;
    assert(!resolver.resolve(0, 17, line, targetUnitId));
    PortConfig manual{};
    manual.enabled = true;
    manual.tcpPort = 502;
    manual.serialSource = SerialSource::Manual;
    manual.manualLine.baud = 9600;
    resolver.configure(0, manual);
    assert(resolver.resolve(0, 17, line, targetUnitId) && line.baud == 9600);
    assert(targetUnitId == 17);
    assert(!resolver.resolve(0, 0, line, targetUnitId));
    assert(!resolver.resolve(3, 17, line, targetUnitId));
    PortConfig device{};
    device.enabled = true;
    device.tcpPort = 503;
    device.serialSource = SerialSource::FlowioDevice;
    device.deviceId = 7;
    resolver.configure(1, device);
    assert(!resolver.resolve(1, 17, line, targetUnitId));
    unsigned calls = 0;
    resolver.setDeviceLookup(&lookup, &calls);
    assert(resolver.resolve(1, 17, line, targetUnitId));
    assert(line.baud == 19200 && line.parity == 1 && line.stopBits == 2);
    assert(calls == 1 && targetUnitId == 11);
    assert(resolver.resolve(1, 0, line, targetUnitId) && targetUnitId == 11);
    device.deviceId = 0;
    resolver.configure(1, device);
    assert(resolver.resolve(1, 17, line, targetUnitId));
    assert(line.baud == 19200 && targetUnitId == 11);  // Device index zero is valid.

    device.deviceId = 8;
    resolver.configure(1, device);
    assert(!resolver.resolve(1, 17, line, targetUnitId));
    device.enabled = false;
    resolver.configure(1, device);
    assert(!resolver.resolve(1, 17, line, targetUnitId));
}

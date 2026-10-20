#include "Core/I2cStartupReadiness.h"
#include <cassert>

int main()
{
    // Reproduce loss of ACK after discovery, then recovery after four probes.
    I2cStartupDevice devices[]{{0x20}, {0x40}, {0x48}, {0}};
    uint32_t clock = 0;
    unsigned attempts = 0;
    awaitI2cStartupDevices(devices, 4, 250,
        [&](uint8_t address, uint32_t) {
            assert(address != 0);
            return ++attempts <= 4 ? I2cProbeResult::Absent : I2cProbeResult::Present;
        }, [&] { return clock; }, [&](uint32_t) { clock += 2; });
    assert(devices[0].ready && devices[1].ready && devices[2].ready);
    assert(!devices[3].ready && attempts == 7 && clock < 250);

    // A persistent failure remains visible and doesn't block healthy devices.
    clock = 0;
    unsigned healthyProbes = 0;
    awaitI2cStartupDevices(devices, 4, 20,
        [&](uint8_t address, uint32_t) {
            if (address == 0x20) return I2cProbeResult::Error;
            ++healthyProbes;
            return I2cProbeResult::Present;
        }, [&] { return clock; }, [&](uint32_t) { clock += 2; });
    assert(!devices[0].ready && devices[1].ready && devices[2].ready);
    assert(clock == 20 && healthyProbes == 2);

    // Transaction and lock durations consume the shared budget, even on success.
    clock = 0;
    attempts = 0;
    awaitI2cStartupDevices(devices, 4, 10,
        [&](uint8_t, uint32_t remaining) {
            assert(remaining == 10);
            clock += remaining;
            ++attempts;
            return I2cProbeResult::Present;
        }, [&] { return clock; }, [&](uint32_t) { assert(false); });
    assert(attempts == 1 && devices[0].ready && !devices[1].ready);

    // The timeout calculation must also work across millis() rollover.
    clock = UINT32_MAX - 3;
    const uint32_t start = clock;
    awaitI2cStartupDevices(devices, 1, 10,
        [](uint8_t, uint32_t) { return I2cProbeResult::Absent; },
        [&] { return clock; }, [&](uint32_t) { clock += 2; });
    assert(clock - start == 10 && !devices[0].ready);

    // No selected hardware means no transaction and no wait.
    I2cStartupDevice none[]{{0}, {0xFF}};
    awaitI2cStartupDevices(none, 2, 250,
        [](uint8_t, uint32_t) { assert(false); return I2cProbeResult::Error; },
        [] { return 0U; }, [](uint32_t) { assert(false); });
}

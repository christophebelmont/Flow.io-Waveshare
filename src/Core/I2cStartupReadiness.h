#pragma once

#include "I2cAddressSelection.h"

struct I2cStartupDevice {
    uint8_t address = 0; // Zero means not selected by address discovery.
    bool ready = false;
};

/** Revalidate discovered devices before initializing providers.
 * Only address probes are retried, never device writes or output commands.
 * One shared deadline bounds the whole plan, including failed bus locks.
 * Probe receives the remaining budget and must bound its own transaction.
 */
template <typename Probe, typename Clock, typename Yield>
void awaitI2cStartupDevices(I2cStartupDevice* devices, size_t count,
                           uint32_t budgetMs, Probe probe, Clock now, Yield yield)
{
    for (size_t i = 0; i < count; ++i) devices[i].ready = false;
    const uint32_t started = now();
    for (;;) {
        bool pending = false;
        for (size_t i = 0; i < count; ++i) {
            auto& device = devices[i];
            if (!validI2cDeviceAddress(device.address) || device.ready) continue;
            const uint32_t elapsed = now() - started;
            if (elapsed >= budgetMs) return;
            device.ready = probe(device.address, budgetMs - elapsed) == I2cProbeResult::Present;
            pending |= !device.ready;
        }
        if (!pending) return;
        const uint32_t elapsed = now() - started;
        if (elapsed >= budgetMs) return;
        yield(budgetMs - elapsed);
    }
}

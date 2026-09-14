#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/** Format a command escaped for its JSON-string field in HA discovery. */
inline bool formatPoolDeviceHaWritePayload(char* out, size_t capacity, uint8_t slot, bool on)
{
    if (!out || !capacity) return false;
    const int written = snprintf(out, capacity,
        "{\\\"cmd\\\":\\\"poollogic.device.write\\\",\\\"args\\\":{\\\"slot\\\":%u,\\\"value\\\":%s}}",
        (unsigned)slot, on ? "true" : "false");
    return written > 0 && (size_t)written < capacity;
}

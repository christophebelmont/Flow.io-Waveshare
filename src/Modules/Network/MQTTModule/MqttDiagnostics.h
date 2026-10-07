#pragma once

#include <stddef.h>
#include <stdint.h>

// Diagnostic vocabulary only: never used to route or admit a publication.
namespace MqttDiagnostics {
struct Source {
    uint8_t producerId;
    uint8_t messageTypeId;
    uint16_t firstMessage;
    uint16_t lastMessage;
    const char* name;
};
inline constexpr Source Sources[] = {
    {1, 1, 0, UINT16_MAX, "ack.reply"},
    {2, 1, 1, 1, "status.online"},
    {2, 2, 32, UINT16_MAX, "status.periodic"},
    {4, 1, 0, UINT16_MAX, "runtime.snapshot"},
    {5, 1, 1, 1, "alarm.meta"},
    {5, 2, 2, 2, "alarm.pack"},
    {5, 3, 100, UINT16_MAX, "alarm.state"},
    {32, 1, 0, UINT16_MAX, "ha.discovery"},
    {41, 1, 0, UINT16_MAX, "config.mqtt"},
    {42, 1, 0, UINT16_MAX, "config.wifi"},
    {43, 1, 0, UINT16_MAX, "config.time"},
    {44, 1, 0, UINT16_MAX, "config.poollogic"},
    {45, 1, 0, UINT16_MAX, "config.system"},
    {46, 1, 0, UINT16_MAX, "config.alarm"},
    {47, 1, 0, UINT16_MAX, "config.io"},
    {48, 1, 0, UINT16_MAX, "config.pooldevice"},
    {49, 1, 0, UINT16_MAX, "config.ha"},
    {52, 1, 0, UINT16_MAX, "config.loghub"},
    {54, 1, 0, UINT16_MAX, "config.display"},
    {0, 0, 0, UINT16_MAX, "unregistered"},
};
inline constexpr size_t SourceCount = sizeof(Sources) / sizeof(Sources[0]);
inline size_t sourceIndex(uint8_t producer, uint16_t message)
{
    for (size_t i = 0; i + 1 < SourceCount; ++i) {
        const Source& source = Sources[i];
        if (source.producerId == producer && message >= source.firstMessage && message <= source.lastMessage) return i;
    }
    return SourceCount - 1;
}
} // namespace MqttDiagnostics

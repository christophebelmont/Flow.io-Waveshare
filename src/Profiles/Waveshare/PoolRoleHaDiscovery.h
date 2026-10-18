#pragma once

#include "Core/Services/IPoolConfiguration.h"
#include "Core/Services/IHA.h"
#include "Core/MqttTopics.h"
#include "Domain/Pool/PoolIds.h"
#include "Profiles/Waveshare/PoolDeviceHaCommand.h"

namespace PoolRoleHaDiscovery {

// Role identity is stable; only the device address follows PoolLogic at boot.
inline uint8_t assignedSlot(PoolDeviceId role, const PoolDeviceAssignments& assignments)
{
    switch (role) {
        case PoolIds::DeviceFiltrationPump: return assignments.filtration;
        case PoolIds::DevicePhPump: return assignments.phPump;
        case PoolIds::DeviceChlorinePump: return assignments.disinfectionPump;
        case PoolIds::DeviceRobot: return assignments.robot;
        case PoolIds::DeviceFillPump: return assignments.filling;
        case PoolIds::DeviceChlorineGenerator: return assignments.chlorineGenerator;
        case PoolIds::DeviceWaterHeater: return assignments.heater;
        // Lighting has no configurable PoolLogic role assignment.
        case PoolIds::DeviceLights: return PoolIds::DeviceLights;
        default: return POOL_DEVICE_INVALID;
    }
}

// HAService retains pointers. The profile owns these buffers until discovery ends.
struct RoleBuffers {
    uint8_t slot = POOL_DEVICE_INVALID;
    char state[24]{};
    char config[16]{};
    char on[128]{};
    char off[128]{};
    char flow[128]{};
    char maxUptime[160]{};
    char refill[80]{};
    char reset[80]{};
};

struct Storage {
    RoleBuffers roles[PoolIds::DeviceCount]{};
};

template <size_t N, typename... Args>
inline bool format(char (&out)[N], const char* pattern, Args... args)
{
    const int written = snprintf(out, N, pattern, args...);
    return written >= 0 && size_t(written) < N;
}

inline bool prepare(Storage& storage, const PoolDeviceAssignments& assignments, uint8_t deviceCapacity)
{
    for (uint8_t role = 0; role < PoolIds::DeviceCount; ++role) {
        const uint8_t slot = assignedSlot(role, assignments);
        if (slot >= deviceCapacity) return false;
        auto& out = storage.roles[role];
        out.slot = slot;
        if (!format(out.state, "rt/pdm/state/pd%u", unsigned(slot)) ||
            !format(out.config, "cfg/pdm/pd%u", unsigned(slot)) ||
            !formatPoolDeviceHaWritePayload(out.on, sizeof(out.on), slot, true) ||
            !formatPoolDeviceHaWritePayload(out.off, sizeof(out.off), slot, false) ||
            !format(out.flow, "{\\\"pdm/pd%u\\\":{\\\"flow_l_h\\\":{{ value | float(0) }}}}", unsigned(slot)) ||
            !format(out.maxUptime, "{\\\"pdm/pd%u\\\":{\\\"max_uptime_day_s\\\":{{ (value | float(0) * 60) | round(0) | int(0) }}}}", unsigned(slot)) ||
            !format(out.refill, "{\"cmd\":\"pool.refill\",\"args\":{\"slot\":%u}}", unsigned(slot)) ||
            !format(out.reset, "{\"cmd\":\"pooldevice.uptime.reset\",\"args\":{\"slot\":%u}}", unsigned(slot))) return false;
    }
    return true;
}

struct NumbersSpec {
    PoolDeviceId role;
    HANumberEntry entry;
    enum class Payload { Flow, MaxUptime } payload;
};

inline constexpr NumbersSpec kNumbers[] = {
    {PoolIds::DeviceFiltrationPump, {
        "pooldev", "pd0_flow", "Filtration Pump Flowrate",
        nullptr, "{{ value_json.flow_l_h }}",
        MqttTopics::SuffixCfgSet, nullptr,
        0.0f, 3.0f, 0.1f, "slider", "config", "mdi:water-sync", "L/h"
    }, NumbersSpec::Payload::Flow},
    {PoolIds::DeviceFiltrationPump, {
        "pooldev", "pd0_max_upt", "Max Uptime Filtration Pump",
        nullptr, "{{ ((value_json.max_uptime_day_s | float(0)) / 60) | round(0) | int(0) }}",
        MqttTopics::SuffixCfgSet, nullptr,
        0.0f, 1440.0f, 1.0f, "box", "config", "mdi:timer-cog-outline", "mn"
    }, NumbersSpec::Payload::MaxUptime},
    {PoolIds::DevicePhPump, {
        "pooldev", "pd1_flow", "pH Pump Flowrate",
        nullptr, "{{ value_json.flow_l_h }}",
        MqttTopics::SuffixCfgSet, nullptr,
        0.0f, 3.0f, 0.1f, "slider", "config", "mdi:water-sync", "L/h"
    }, NumbersSpec::Payload::Flow},
    {PoolIds::DeviceChlorinePump, {
        "pooldev", "pd2_flow", "Chlorine Pump Flowrate",
        nullptr, "{{ value_json.flow_l_h }}",
        MqttTopics::SuffixCfgSet, nullptr,
        0.0f, 3.0f, 0.1f, "slider", "config", "mdi:water-sync", "L/h"
    }, NumbersSpec::Payload::Flow},
    {PoolIds::DevicePhPump, {
        "pooldev", "pd1_max_upt", "Max Uptime pH Pump",
        nullptr, "{{ ((value_json.max_uptime_day_s | float(0)) / 60) | round(0) | int(0) }}",
        MqttTopics::SuffixCfgSet, nullptr,
        1.0f, 120.0f, 1.0f, "box", "config", "mdi:timer-cog-outline", "mn"
    }, NumbersSpec::Payload::MaxUptime},
    {PoolIds::DeviceChlorinePump, {
        "pooldev", "pd2_max_upt", "Max Uptime Chlorine Pump",
        nullptr, "{{ ((value_json.max_uptime_day_s | float(0)) / 60) | round(0) | int(0) }}",
        MqttTopics::SuffixCfgSet, nullptr,
        1.0f, 120.0f, 1.0f, "box", "config", "mdi:timer-cog-outline", "mn"
    }, NumbersSpec::Payload::MaxUptime},
    {PoolIds::DeviceFillPump, {
        "pooldev", "pd4_max_upt", "Max Uptime Fill Pump",
        nullptr, "{{ ((value_json.max_uptime_day_s | float(0)) / 60) | round(0) | int(0) }}",
        MqttTopics::SuffixCfgSet, nullptr,
        0.0f, 120.0f, 1.0f, "box", "config", "mdi:timer-cog-outline", "mn"
    }, NumbersSpec::Payload::MaxUptime},
    {PoolIds::DeviceChlorineGenerator, {
        "pooldev", "pd5_max_upt", "Max Uptime Chlorine Generator",
        nullptr, "{{ ((value_json.max_uptime_day_s | float(0)) / 60) | round(0) | int(0) }}",
        MqttTopics::SuffixCfgSet, nullptr,
        0.0f, 1440.0f, 1.0f, "box", "config", "mdi:timer-cog-outline", "mn"
    }, NumbersSpec::Payload::MaxUptime},
};

struct ButtonsSpec {
    PoolDeviceId role;
    HAButtonEntry entry;
    enum class Payload { Refill, Reset, None } payload;
};

inline constexpr ButtonsSpec kButtons[] = {
    {PoolIds::DevicePhPump, {
        "pooldev",
        "pd_refill_ph",
        "Fill pH Tank",
        MqttTopics::SuffixCmd,
        nullptr,
        "config",
        "mdi:beaker-plus-outline"
    }, ButtonsSpec::Payload::Refill},
    {PoolIds::DeviceChlorinePump, {
        "pooldev",
        "pd_refill_chl",
        "Fill Chlorine Tank",
        MqttTopics::SuffixCmd,
        nullptr,
        "config",
        "mdi:water-plus"
    }, ButtonsSpec::Payload::Refill},
    {PoolIds::DeviceFiltrationPump, {
        "pooldev",
        "pd_reset_upt_flt",
        "Reset Uptime Filtration Pump",
        MqttTopics::SuffixCmd,
        nullptr,
        "diagnostic",
        "mdi:timer-refresh-outline"
    }, ButtonsSpec::Payload::Reset},
    {PoolIds::DevicePhPump, {
        "pooldev",
        "pd_reset_upt_ph",
        "Reset Uptime pH Pump",
        MqttTopics::SuffixCmd,
        nullptr,
        "diagnostic",
        "mdi:timer-refresh-outline"
    }, ButtonsSpec::Payload::Reset},
    {PoolIds::DeviceChlorinePump, {
        "pooldev",
        "pd_reset_upt_chl",
        "Reset Uptime Chlorine Pump",
        MqttTopics::SuffixCmd,
        nullptr,
        "diagnostic",
        "mdi:timer-refresh-outline"
    }, ButtonsSpec::Payload::Reset},
    {PoolIds::DeviceFillPump, {
        "pooldev",
        "pd_reset_upt_fill",
        "Reset Uptime Fill Pump",
        MqttTopics::SuffixCmd,
        nullptr,
        "diagnostic",
        "mdi:timer-refresh-outline"
    }, ButtonsSpec::Payload::Reset},
    {PoolIds::DeviceChlorineGenerator, {
        "pooldev",
        "pd_reset_upt_chl_gen",
        "Reset Uptime Chlorine Generator",
        MqttTopics::SuffixCmd,
        nullptr,
        "diagnostic",
        "mdi:timer-refresh-outline"
    }, ButtonsSpec::Payload::Reset},
    {PoolIds::DeviceFiltrationPump, {
        "pooldev",
        "pd_reset_upt_all",
        "Reset Uptime All Pool Devices",
        MqttTopics::SuffixCmd,
        "{\"cmd\":\"pooldevice.uptime.reset_all\"}",
        "diagnostic",
        "mdi:timer-refresh-outline"
    }, ButtonsSpec::Payload::None},
};

inline bool registerEntries(const HAService& ha, const Storage& storage)
{
    if (!ha.addNumber || !ha.addButton) return false;
    bool ok = true;
    for (const auto& spec : kNumbers) {
        auto entry = spec.entry;
        const auto& role = storage.roles[spec.role];
        entry.stateTopicSuffix = role.config;
        entry.commandTemplate = spec.payload == NumbersSpec::Payload::Flow ? role.flow : role.maxUptime;
        if (!ha.addNumber(ha.ctx, &entry)) ok = false;
    }
    for (const auto& spec : kButtons) {
        auto entry = spec.entry;
        const auto& role = storage.roles[spec.role];
        switch (spec.payload) {
            case ButtonsSpec::Payload::Refill: entry.payloadPress = role.refill; break;
            case ButtonsSpec::Payload::Reset: entry.payloadPress = role.reset; break;
            case ButtonsSpec::Payload::None: break;
        }
        if (!ha.addButton(ha.ctx, &entry)) ok = false;
    }
    return ok;
}

} // namespace PoolRoleHaDiscovery

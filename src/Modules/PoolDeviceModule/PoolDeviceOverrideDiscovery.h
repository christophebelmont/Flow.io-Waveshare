#pragma once
#include "Core/Services/IHA.h"

namespace PoolDeviceOverrideDiscovery {
inline constexpr const char* Selected = "rt/pdm/override/selected";
inline constexpr const char* All = "rt/pdm/override/all";
inline constexpr const char* Available = "{{ 'online' if value_json.override_available else 'offline' }}";
inline constexpr const char* Supported = "{{ 'online' if value_json.override_supported else 'offline' }}";
inline constexpr const char* Releasable = "{{ 'online' if value_json.control_mode != 'guided' else 'offline' }}";
inline constexpr const char* State = "{{ ('forced_on' if value_json.override_value else 'forced_off') if value_json.control_mode == 'forced' else value_json.control_mode }}";
inline const HANumberEntry Duration = {
    "pooldevice", "pdm_ovr_min", "Override Duration", Selected, "{{ value_json.duration_minutes }}", "cmd",
    "{\\\"cmd\\\":\\\"pooldevice.override.duration.set\\\",\\\"args\\\":{\\\"minutes\\\":{{ value | int }}}}",
    1, 1440, 1, "box", nullptr, "mdi:timer-outline", "min"
};
inline const HASensorEntry Sensors[] = {
    {"pooldevice", "pdm_ovr_state", "Selected Override State", Selected, State, nullptr,
     "mdi:hand-back-right-outline", nullptr, false, Supported, true, "{{ value_json | to_json }}"},
    {"pooldevice", "pdm_ovr_active", "Active Overrides", All, "{{ value_json.active_count }}", nullptr,
     "mdi:hand-back-right-outline", nullptr, false, nullptr, false, "{{ {'actuators': value_json.actuators} | to_json }}"},
};
inline const HAButtonEntry Buttons[] = {
    {"pooldevice", "pdm_ovr_on", "Force On", "cmd", "{\"cmd\":\"pooldevice.override_on\"}", nullptr,
     "mdi:play", Selected, Available, false},
    {"pooldevice", "pdm_ovr_off", "Force Off", "cmd", "{\"cmd\":\"pooldevice.override_off\"}", nullptr,
     "mdi:stop", Selected, Available, false},
    {"pooldevice", "pdm_ovr_release", "End Override", "cmd", "{\"cmd\":\"pooldevice.release\"}", nullptr,
     "mdi:autorenew", Selected, Releasable, false},
};
}

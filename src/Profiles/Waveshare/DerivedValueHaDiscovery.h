#pragma once

#include "Core/Services/IHA.h"
#include "Core/Values/Value.h"
#include <stdio.h>

namespace DerivedValueHaDiscovery {

// HA retains these strings until discovery publication completes.
struct SlotBuffers {
    char objectSuffix[16]{};
    char name[24]{};
    char stateTopic[24]{};
};
struct Storage {
    SlotBuffers slots[ValueIds::DerivedCapacity]{};
};

inline bool registerEntry(const HAService& service, Storage& storage, uint8_t slot, bool published,
                          const char* name = nullptr, const char* unit = nullptr)
{
    if (slot >= ValueIds::DerivedCapacity) return false;
    auto& out = storage.slots[slot];
    snprintf(out.objectSuffix, sizeof(out.objectSuffix), "io_value_v%02u", unsigned(slot));
    if (!published) {
        const HADiscoveryRemovalEntry entry{"sensor", out.objectSuffix};
        return service.addDiscoveryRemoval && service.addDiscoveryRemoval(service.ctx, &entry);
    }
    snprintf(out.name, sizeof(out.name), "Value V%02u", unsigned(slot));
    snprintf(out.stateTopic, sizeof(out.stateTopic), "rt/value/%u", unsigned(ValueIds::Derived + slot));
    static_assert(unsigned(ValueQuality::Valid) == 1, "Update the HA quality template when the wire format changes");
    const HASensorEntry entry{
        "io", out.objectSuffix, name && name[0] ? name : out.name, out.stateTopic, "{{ value_json.value }}",
        nullptr, "mdi:function-variant", (unit && unit[0]) ? unit : nullptr, true,
        "{{ 'online' if value_json.quality == 1 else 'offline' }}", false, nullptr, out.name
    };
    return service.addSensor && service.addSensor(service.ctx, &entry);
}

} // namespace DerivedValueHaDiscovery

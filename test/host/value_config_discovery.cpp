#include "Modules/IOModule/ValueConfig.h"
#include "Profiles/Waveshare/DerivedValueHaDiscovery.h"
#include <cassert>
#include <cstring>
#include <vector>

struct Discovery {
    std::vector<HASensorEntry> sensors;
    std::vector<HADiscoveryRemovalEntry> removals;
    HAService service{};
    Discovery() {
        service.ctx = this;
        service.addSensor = [](void* ctx, const HASensorEntry* entry) {
            static_cast<Discovery*>(ctx)->sensors.push_back(*entry); return true;
        };
        service.addDiscoveryRemoval = [](void* ctx, const HADiscoveryRemovalEntry* entry) {
            static_cast<Discovery*>(ctx)->removals.push_back(*entry); return true;
        };
    }
};

int main() {
    ValueConfig config;
    ConfigStore store;
    config.registerConfig(store, 1);
    assert(store.entries.size() == 10 * ValueIds::DerivedCapacity);
    unsigned enabledFields = 0;
    for (const auto& entry : store.entries) {
        if (entry.name == "enabled") {
            assert(entry.type == ConfigType::Bool);
            assert(entry.key.substr(entry.key.size()-2) == "_2");
            ++enabledFields;
        }
    }
    assert(enabledFields == 5);
    ValueRegistry registry;
    assert(registry.begin());
    assert(registry.define(ValueIds::Analog, {}));
    // A configured source alone must not activate a slot.
    for (auto& definition : config.definitions) {
        assert(!definition.enabled);
        std::strcpy(definition.expr, "a00");
    }
    assert(config.resolve(registry));
    ValueSnapshot inactive;
    for (uint8_t slot = 0; slot < ValueIds::DerivedCapacity; ++slot) {
        assert(!registry.read(ValueIds::Derived + slot, inactive));
    }
    config.definitions[0].enabled = true;
    config.definitions[1].enabled = true;
    std::strcpy(config.definitions[0].expr, "a00 * 2");
    std::strcpy(config.definitions[0].unit, "L");
    config.definitions[0].precision = 2;
    std::strcpy(config.definitions[1].expr, "v00 + 1");
    std::strcpy(config.definitions[2].expr, "a00");
    config.definitions[2].enabled = false;
    assert(config.resolve(registry));
    ValueNumber number; number.f = 3;
    assert(registry.write(ValueIds::Analog, number, 10));
    ValueSnapshot snapshot;
    assert(registry.read(ValueIds::Derived+1, snapshot) && snapshot.value.d == 7);
    assert(!registry.read(ValueIds::Derived+2, snapshot));
    // The configured unit and precision travel with the value metadata.
    ValueMetadata metadata;
    assert(registry.read(ValueIds::Derived, snapshot, &metadata));
    assert(std::strcmp(metadata.displayUnit, "L") == 0);
    assert(metadata.precision == 2);

    Discovery discovery;
    DerivedValueHaDiscovery::Storage buffers;
    for (uint8_t slot = 0; slot < ValueIds::DerivedCapacity; ++slot) {
        assert(DerivedValueHaDiscovery::registerEntry(discovery.service, buffers, slot,
            registry.read(ValueIds::Derived+slot, snapshot), nullptr,
            slot == 0 ? "L" : nullptr));
    }
    assert(discovery.sensors.size() == 2 && discovery.removals.size() == 3);
    assert(std::strcmp(discovery.sensors[0].stateTopicSuffix, "rt/value/96") == 0);
    assert(std::strcmp(discovery.sensors[0].unit, "L") == 0);
    assert(std::strcmp(discovery.sensors[1].objectSuffix, "io_value_v01") == 0);
    assert(std::strstr(discovery.sensors[0].availabilityTemplate, "quality == 1"));
    assert(std::strcmp(discovery.removals.back().objectSuffix, "io_value_v04") == 0);
    assert(std::strcmp(discovery.removals.back().component, "sensor") == 0);
    assert(!DerivedValueHaDiscovery::registerEntry(discovery.service, buffers, 5, true));

    // Reboot with V01 disabled removes exactly its previous discovery identity.
    ValueRegistry rebooted;
    assert(rebooted.begin());
    assert(rebooted.define(ValueIds::Analog, {}));
    config.definitions[1].enabled = false;
    assert(config.resolve(rebooted));
    Discovery afterReboot;
    DerivedValueHaDiscovery::Storage rebootBuffers;
    assert(DerivedValueHaDiscovery::registerEntry(afterReboot.service, rebootBuffers, 1,
        rebooted.read(ValueIds::Derived+1, snapshot)));
    assert(afterReboot.sensors.empty());
    assert(std::strcmp(afterReboot.removals[0].objectSuffix, discovery.sensors[1].objectSuffix) == 0);

    // An enabled dependent requires an enabled source; reject invalid topology.
    ValueRegistry invalid;
    assert(invalid.begin());
    config.definitions[0].enabled = false;
    config.definitions[1].enabled = true;
    assert(!config.resolve(invalid));
    config.definitions[1].enabled = false;
    assert(config.resolve(invalid));
    // Custom labels leave the legacy identity and topic unchanged.
    Discovery named;
    assert(DerivedValueHaDiscovery::registerEntry(named.service, buffers, 0, true, "Débit piscine"));
    assert(std::strcmp(named.sensors[0].name, "Débit piscine") == 0);
    assert(std::strcmp(named.sensors[0].identityName, "Value V00") == 0);
    assert(std::strcmp(named.sensors[0].objectSuffix, discovery.sensors[0].objectSuffix) == 0);
    assert(DerivedValueHaDiscovery::registerEntry(named.service, buffers, 0, true, ""));
    assert(std::strcmp(named.sensors[1].name, "Value V00") == 0);
    HAService missing{};
    assert(!DerivedValueHaDiscovery::registerEntry(missing, buffers, 0, true));
    assert(!DerivedValueHaDiscovery::registerEntry(missing, buffers, 0, false));
}

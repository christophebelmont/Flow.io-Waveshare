#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#include "Core/Control/ActuatorOverrideDuration.h"
#include "Core/Control/TimedActuatorOverride.h"
#include "Modules/PoolDeviceModule/PoolDeviceOverrideDiscovery.h"
#include "Modules/Network/HAModule/HADiscoveryJson.h"

struct Storage {
    bool fail = false;
    unsigned writes = 0;
    std::vector<uint8_t> bytes;
    static bool save(void* ctx, const uint8_t* data, size_t length) {
        auto& storage = *static_cast<Storage*>(ctx);
        ++storage.writes;
        if (storage.fail) return false;
        storage.bytes.assign(data, data + length);
        return true;
    }
};
int main() {
    Storage preference;
    ActuatorOverrideDuration duration;
    assert(duration.minutes() == 30 && duration.seconds() == 1800);
    assert(!duration.set(0, &preference, Storage::save));
    assert(!duration.set(1441, &preference, Storage::save));
    assert(duration.set(30, &preference, Storage::save) && preference.writes == 0);
    assert(duration.set(1440, &preference, Storage::save));
    assert(duration.seconds() == 86400);
    ActuatorOverrideDuration reboot;
    assert(reboot.restore(preference.bytes.data(), preference.bytes.size()));
    assert(reboot.minutes() == 1440);
    preference.fail = true;
    assert(!duration.set(15, &preference, Storage::save));
    assert(duration.minutes() == 1440);
    preference.bytes[0] = 99;
    assert(!reboot.restore(preference.bytes.data(), preference.bytes.size()));
    assert(!reboot.restore(nullptr, 4));
    const uint8_t zero[] = {1, 0, 0, 0};
    assert(!reboot.restore(zero, sizeof(zero)));
    const uint8_t oversized[] = {1, 0, 0xff, 0xff};
    assert(!reboot.restore(oversized, sizeof(oversized)));

    Storage leaseStorage;
    TimedActuatorOverride lease;
    lease.configure(&leaseStorage, Storage::save);
    assert(lease.start(5, 2, true, reboot.seconds(), 1000, 0));
    preference.fail = false;
    assert(reboot.set(15, &preference, Storage::save));
    assert(lease.state(0).remainingSec == 86400); // Next duration does not shorten the current lease.

    using namespace PoolDeviceOverrideDiscovery;
    
    assert(sizeof(Sensors) / sizeof(*Sensors) == 2);
    assert(sizeof(Buttons) / sizeof(*Buttons) == 3);
    for (size_t i = 0; i < 3; ++i) {
        const auto& entry = Buttons[i];
        DynamicJsonDocument discovery(4096), payload(512);
        auto root = discovery.to<JsonObject>();
        HADiscoveryJson::button(root, "flow/device/cmd", entry.payloadPress);
        HADiscoveryJson::availability(root, "flow/device/status", entry.availabilityTopicSuffix, entry.availabilityTemplate);
        char bytes[4096];
        assert(HADiscoveryJson::serialize(discovery, bytes, sizeof(bytes)));
        assert(!deserializeJson(discovery, bytes));
        assert(discovery["ret"] == false);
        assert(discovery["avty_mode"] == "all");
        assert(discovery["avty"].size() == 2);
        assert(!deserializeJson(payload, discovery["pl_prs"].as<const char*>()));
        assert(!payload.containsKey("args"));
        assert(!payload["args"].containsKey("slot"));
        assert(!payload["args"].containsKey("duration_s"));
        assert(entry.includeNameInUniqueId == false);
        assert(strcmp(entry.availabilityTemplate, i % 3 == 2 ? Releasable : Available) == 0);
    }
    for (const auto& entry : Sensors) {
        DynamicJsonDocument doc(4096);
        HADiscoveryJson::sensor(doc.to<JsonObject>(), entry.stateTopicSuffix, entry.valueTemplate,
                               entry.isText, entry.attributesTemplate);
        char encoded[4096];
        assert(HADiscoveryJson::serialize(doc, encoded, sizeof(encoded)));
        assert(!deserializeJson(doc, encoded));
        assert(strcmp(doc["val_tpl"], entry.valueTemplate) == 0);
        if (entry.isText) {
            assert(!doc.containsKey("stat_cla"));
            assert(strcmp(doc["json_attr_tpl"], entry.attributesTemplate) == 0);
            assert(strcmp(doc["json_attr_t"], entry.stateTopicSuffix) == 0);
            assert(strstr(entry.valueTemplate, "forced_on"));
            assert(!strstr(entry.valueTemplate, "Marche"));
        } else {
            assert(doc["stat_cla"] == "measurement");
            assert(strstr(doc["json_attr_tpl"].as<const char*>(), "actuators"));
        }
    }
    // Templates containing quotes and newlines remain intact in JSON.
    {
        DynamicJsonDocument doc(4096);
        const char* templ = "{{ value_json[\"quoted\"] }}\n";
        HADiscoveryJson::sensor(doc.to<JsonObject>(), "state", templ, true);
        char encoded[4096];
        assert(HADiscoveryJson::serialize(doc, encoded, sizeof(encoded)));
        assert(!deserializeJson(doc, encoded));
        assert(strcmp(doc["val_tpl"], templ) == 0);
    }
    for (const auto& entry : {Duration}) {
        // Exercise the escaping expected by the existing number serializer.
        std::string json = std::string("{\"cmd_tpl\":\"") + entry.commandTemplate + "\"}";
        DynamicJsonDocument doc(2048);
        assert(!deserializeJson(doc, json));
        std::string command = doc["cmd_tpl"].as<std::string>();
        const std::string expression = "{{ value | int }}";
        command.replace(command.find(expression), expression.size(), "45");
        assert(!deserializeJson(doc, command));
        assert(strcmp(doc["cmd"], "pooldevice.override.duration.set") == 0);
        assert(doc["args"]["minutes"] == 45);
        assert(entry.minValue == 1 && entry.maxValue == 1440);
    }
}

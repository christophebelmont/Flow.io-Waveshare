#include "Profiles/Waveshare/PoolRoleHaDiscovery.h"
#include <ArduinoJson.h>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

struct Capture {
    std::vector<HASensorEntry> sensors;
    std::vector<HADiscoveryRemovalEntry> removals;
    std::vector<HANumberEntry> numbers;
    std::vector<HAButtonEntry> buttons;
    bool accept = true;
    HAService service() {
        HAService ha{};
        ha.ctx = this;
        ha.addDiscoveryRemoval = [](void* ctx, const HADiscoveryRemovalEntry* entry) {
            auto& self = *static_cast<Capture*>(ctx);
            self.removals.push_back(*entry); return self.accept;
        };
        ha.addSensor = [](void* ctx, const HASensorEntry* entry) {
            auto& self = *static_cast<Capture*>(ctx);
            self.sensors.push_back(*entry); return self.accept;
        };
        ha.addNumber = [](void* ctx, const HANumberEntry* entry) {
            auto& self = *static_cast<Capture*>(ctx);
            self.numbers.push_back(*entry); return self.accept;
        };
        ha.addButton = [](void* ctx, const HAButtonEntry* entry) {
            auto& self = *static_cast<Capture*>(ctx);
            self.buttons.push_back(*entry); return self.accept;
        };
        return ha;
    }
};

static std::string unescape(const char* value) {
    DynamicJsonDocument doc(2048);
    const auto quoted = std::string("\"") + value + "\"";
    assert(!deserializeJson(doc, quoted));
    return doc.as<std::string>();
}

int main() {
    using namespace PoolRoleHaDiscovery;
    const PoolDeviceAssignments defaults{0, 1, 2, 3, 4, 5, 7};
    Storage oldStorage{};
    assert(prepare(oldStorage, defaults, 16));
    Capture old;
    assert(registerEntries(old.service(), oldStorage));
    assert(old.sensors.empty() && old.numbers.size() == 8 && old.buttons.size() == 8);
    assert(old.removals.empty());
    assert(std::strcmp(old.numbers[0].objectSuffix, "pd0_flow") == 0);
    assert(std::strcmp(old.buttons[0].objectSuffix, "pd_refill_ph") == 0);

    // Exercise every configured role on every new slot, including the upper boundary.
    for (uint8_t slot = 8; slot < 16; ++slot) {
        PoolDeviceAssignments assignments{slot, slot, slot, slot, slot, slot, slot};
        Storage storage{};
        assert(prepare(storage, assignments, 16));
        Capture captured;
        assert(registerEntries(captured.service(), storage));
        assignments = defaults; // Discovery remains frozen after registration.
        const auto id = std::string("pd") + std::to_string(slot);
        for (uint8_t role = 0; role < PoolIds::DeviceCount; ++role) {
            const auto& buffers = storage.roles[role];
            const auto expected = role == PoolIds::DeviceLights ? 6 : slot;
            assert(buffers.slot == expected);
            assert(std::string(buffers.state) == "rt/pdm/state/pd" + std::to_string(expected));
            for (const auto* payload : {buffers.on, buffers.off}) {
                DynamicJsonDocument doc(512);
                assert(!deserializeJson(doc, unescape(payload)));
                assert(doc["cmd"] == "poollogic.device.write");
                assert(doc["args"]["slot"] == expected);
                assert(doc["args"]["value"] == (payload == buffers.on));
            }
        }
        assert(captured.sensors.empty());
        for (size_t i = 0; i < captured.numbers.size(); ++i) {
            const auto& entry = captured.numbers[i];
            assert(std::strcmp(entry.objectSuffix, old.numbers[i].objectSuffix) == 0);
            assert(std::strcmp(entry.name, old.numbers[i].name) == 0);
            assert(std::string(entry.stateTopicSuffix) == "cfg/pdm/" + id);
            auto command = unescape(entry.commandTemplate);
            const auto start = command.find("{{");
            const auto end = command.find("}}", start);
            assert(start != std::string::npos && end != std::string::npos);
            command.replace(start, end + 2 - start, "12");
            DynamicJsonDocument doc(512);
            assert(!deserializeJson(doc, command));
            assert(doc.containsKey("pdm/" + id));
        }
        for (size_t i = 0; i < captured.buttons.size(); ++i) {
            const auto& entry = captured.buttons[i];
            assert(std::strcmp(entry.objectSuffix, old.buttons[i].objectSuffix) == 0);
            assert(std::strcmp(entry.name, old.buttons[i].name) == 0);
            DynamicJsonDocument doc(512);
            assert(!deserializeJson(doc, entry.payloadPress));
            if (doc["cmd"] == "pooldevice.uptime.reset_all") assert(!doc.containsKey("args"));
            else assert(doc["args"]["slot"] == slot);
        }
    }
    // Mixed mappings must not leak one role's buffers into another role.
    Storage mixed{};
    assert(prepare(mixed, {15, 14, 13, 12, 11, 10, 9}, 16));
    const uint8_t expected[] = {15, 14, 13, 12, 11, 10, 6, 9};
    for (uint8_t role = 0; role < PoolIds::DeviceCount; ++role) assert(mixed.roles[role].slot == expected[role]);
    Storage invalid{};
    assert(!prepare(invalid, PoolDeviceAssignments{}, 16));
    assert(!prepare(invalid, {16, 1, 2, 3, 4, 5, 7}, 16));
    assert(!prepare(invalid, {8, 1, 2, 3, 4, 5, 7}, 8));
    assert(assignedSlot(255, defaults) == POOL_DEVICE_INVALID);
    Capture rejected; rejected.accept = false;
    assert(!registerEntries(rejected.service(), oldStorage));
    assert(!registerEntries(HAService{}, oldStorage));
}

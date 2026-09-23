"""Native regression checks for production alarm discovery and reset semantics.

Compile the actual AlarmModule method bodies against a small host runtime; use
ArduinoJson to round-trip the discovery payload and its nested MQTT command.
"""
from pathlib import Path
import json
import re
import subprocess
import tempfile
import unittest

from scripts.tests.test_spiram_json_document import ARDUINO_JSON, HEAP_HEADER

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = source.index(name)
    start = source.rfind('\n', 0, start) + 1
    end = source.index('\n}', start) + 2
    return source[start:end]


PREAMBLE = r'''
#include "Core/Services/IAlarm.h"
#include "Core/Services/IHA.h"
#include "Core/SpiRamJsonDocument.h"
#include "Modules/Network/HAModule/HADiscoveryJson.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
namespace Limits { namespace Alarm { constexpr unsigned MaxAlarms = 16; } }
namespace MqttTopics { constexpr const char* SuffixCmd = "cmd"; }
enum class ServiceId { Ha };
struct ServiceRegistry { template<class T> T* get(ServiceId) { return nullptr; } };
struct Actor {};
enum class EventId { AlarmReset, AlarmCleared };
enum class ActivityCode { AlarmReset };
template<class... Args> void logNoop(Args...) {}
#define LOGI(...) logNoop(__VA_ARGS__)
#define LOGD(...) logNoop(__VA_ARGS__)
#define LOGW(...) logNoop(__VA_ARGS__)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
uint32_t millis() { return 12345; }
void* heap_caps_calloc(size_t n, size_t size, unsigned caps) {
    void* p = heap_caps_malloc(n * size, caps);
    if (p) memset(p, 0, n * size);
    return p;
}
struct Captured {
    std::vector<HASensorEntry> sensors;
    std::vector<HABinarySensorEntry> binary;
    std::vector<HAButtonEntry> buttons;
    std::vector<HADiscoveryRemovalEntry> removals;
    bool rejectBinary = false;
    HAService service{};
    Captured() {
        service.ctx = this;
        service.addSensor = [](void* ctx, const HASensorEntry* e) {
            static_cast<Captured*>(ctx)->sensors.push_back(*e); return true;
        };
        service.addBinarySensor = [](void* ctx, const HABinarySensorEntry* e) {
            auto& c = *static_cast<Captured*>(ctx);
            if (c.rejectBinary) return false;
            c.binary.push_back(*e); return true;
        };
        service.addButton = [](void* ctx, const HAButtonEntry* e) {
            static_cast<Captured*>(ctx)->buttons.push_back(*e); return true;
        };
        service.addDiscoveryRemoval = [](void* ctx, const HADiscoveryRemovalEntry* e) {
            static_cast<Captured*>(ctx)->removals.push_back(*e); return true;
        };
    }
};
'''

CLASS = r'''
    AlarmSlot slots_[Limits::Alarm::MaxAlarms]{};
    HaAlarmDiscovery* haAlarms_[Limits::Alarm::MaxAlarms]{};
    const HAService* haSvc_ = nullptr;
    bool haEntitiesRegistered_ = false;
    mutable int slotsMux_ = 0;
    int events = 0;
    int activities = 0;
    void registerHaEntities_(ServiceRegistry&);
    int16_t findSlotById_(AlarmId) const;
    bool buildAlarmState_(AlarmId, char*, size_t) const;
    bool resetWithActor_(AlarmId, const Actor&);
    uint8_t resetAllWithActor_(const Actor&);
    static const char* condStateStr_(AlarmCondState);
    void emitAlarmEvent_(EventId, AlarmId) { ++events; }
    void emitAlarmActivity_(ActivityCode, AlarmId, const Actor&) { ++activities; }
    ~AlarmModule() { for (auto* p : haAlarms_) heap_caps_free(p); }
};
'''

MQTT_RUNTIME = r'''
enum class MqttBuildResult { RetryLater, PermanentError, Ready, NoLongerNeeded };
struct MqttBuildContext {
    char* topic; size_t topicCapacity; char* payload; size_t payloadCapacity;
    uint16_t topicLen = 0; uint16_t payloadLen = 0; uint8_t qos = 0; bool retain = false; bool allowEmptyPayload = false;
};
class MQTTModule { public:
    static constexpr uint16_t AlarmMsgMeta = 1, AlarmMsgPack = 2, AlarmMsgStateBase = 100;
    const AlarmService* alarmSvc_ = nullptr;
    struct { const char* baseTopic = "flow"; } cfgData_;
    const char* deviceId_ = "test";
    MqttBuildResult buildAlarm_(uint16_t, MqttBuildContext&);
};
void checkMqttState(AlarmModule& module) {
    AlarmService service{};
    service.ctx = &module;
    service.activeCount = [](void*) -> uint8_t { return 2; };
    service.highestSeverity = [](void*) { return AlarmSeverity::Critical; };
    service.listIds = [](void*, AlarmId* out, uint8_t max) -> uint8_t {
        assert(max >= 2); out[0] = AlarmId::PoolPsiLow; out[1] = AlarmId::PoolPsiHigh; return 2;
    };
    service.isResettable = [](void*, AlarmId id) { return id == AlarmId::PoolPsiLow; };
    service.buildAlarmState = [](void* ctx, AlarmId id, char* out, size_t len) {
        return static_cast<AlarmModule*>(ctx)->buildAlarmState_(id, out, len);
    };
    MQTTModule mqtt;
    mqtt.alarmSvc_ = &service;
    char topic[192]{}, payload[512]{};
    MqttBuildContext context{topic, sizeof(topic), payload, sizeof(payload)};
    assert(mqtt.buildAlarm_(MQTTModule::AlarmMsgMeta, context) == MqttBuildResult::Ready);
    assert(context.retain && std::string(topic) == "flow/test/rt/alarms/m");
    SpiRamJsonDocument state(1024);
    assert(!deserializeJson(state, payload));
    assert(state["a"] == 2 && state["r"] == 1 && state["h"] == 3);
    context.retain = false;
    assert(mqtt.buildAlarm_(MQTTModule::AlarmMsgStateBase + 1000, context) == MqttBuildResult::Ready);
    assert(context.retain && std::string(topic) == "flow/test/rt/alarms/id1000");
    assert(!deserializeJson(state, payload));
    assert(state["id"] == 1000 && state["a"] == 0 && state["r"] == 0);
}
'''

REMOVAL_RUNTIME = r'''
#include <cstdarg>
#define HA_BOOT_TRACE_D(...) logNoop(__VA_ARGS__)
bool formatChecked(char* out, size_t len, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    int wrote = vsnprintf(out, len, fmt, args);
    va_end(args); return wrote >= 0 && static_cast<size_t>(wrote) < len;
}
bool networkReady(int) { return true; }
bool mqttReady(int) { return true; }
void setHaVendor(int&, const char*) {}
void setHaDeviceId(int&, const char*) {}
class HAModule { public:
    bool oneShotCompleted_ = false, startupReady_ = true;
    struct { bool enabled = true; const char* vendor = "test";
             const char* discoveryPrefix = "homeassistant"; } cfgData_;
    struct Mqtt { bool formatTopic = true; } mqtt_;
    Mqtt* mqttSvc_ = &mqtt_;
    int store = 0;
    struct DS { int* store; } ds_{&store};
    DS* dsSvc_ = &ds_;
    const char* deviceId_ = "device";
    const char* nodeTopicId_ = "device";
    char objectIdBuf_[96]{};
    HADiscoveryRemovalEntry removals_[8]{};
    uint8_t removalCount_ = 0;
    bool isPending_(uint16_t) const { return true; }
    void refreshIdentityFromConfig() {}
    uint16_t entityCount_() const { return 3; }
    uint16_t messageCount_() const;
    bool buildObjectId(const char* suffix, char* out, size_t len) const {
        return formatChecked(out, len, "fio_%s", suffix);
    }
    bool buildEntityMessage_(uint16_t, MqttBuildContext&) { return true; }
    bool buildRemovalMessage_(uint16_t, MqttBuildContext&);
    bool publishDiscovery(const char*, const char*, MqttBuildContext&);
    MqttBuildResult buildMessage_(uint16_t, MqttBuildContext&);
};
void checkRemoval() {
    HAModule ha;
    for (auto entry : kLegacyAlarmButtons) ha.removals_[ha.removalCount_++] = entry;
    char topic[192], payload[64];
    for (uint16_t index = 0; index < 8; ++index) {
        strcpy(payload, "old payload must be cleared");
        MqttBuildContext context{topic, sizeof(topic), payload, sizeof(payload)};
        assert(ha.buildMessage_(ha.entityCount_() + index, context) == MqttBuildResult::Ready);
        assert(context.allowEmptyPayload && context.payloadLen == 0 && context.retain);
        assert(std::string(topic) == "homeassistant/button/device/fio_alm_reset_slot_"
                                     + std::to_string(index) + "/config");
    }
    MqttBuildContext context{topic, sizeof(topic), payload, sizeof(payload)};
    assert(ha.buildMessage_(ha.messageCount_(), context) == MqttBuildResult::NoLongerNeeded);
    ha.oneShotCompleted_ = true;
    assert(ha.buildMessage_(ha.entityCount_(), context) == MqttBuildResult::NoLongerNeeded);
}
'''

MAIN = r'''
int main() {
    checkRemoval();
    ServiceRegistry services;
    {
        Captured captured;
        AlarmModule module;
        module.haSvc_ = &captured.service;
        // Sparse registry and IDs deliberately unrelated to slot order.
        for (unsigned index : {2U, 11U}) {
            auto& slot = module.slots_[index];
            slot.used = true;
            slot.id = static_cast<AlarmId>(1400 - index);
            slot.def.latched = index == 2;
            strcpy(slot.def.title, "Pressure \"quoted\" \\ sensor");
        }
        module.registerHaEntities_(services);
        assert(module.haEntitiesRegistered_);
        assert(captured.binary.size() == 3); // two occupied slots + aggregate
        assert(captured.buttons.size() == 3); // two occupied slots + global
        assert(captured.sensors.size() == 2); // compatibility pack + count
        assert(captured.removals.size() == 8);
        assert(std::string(captured.buttons[0].objectSuffix) == "alm_reset_all");
        for (unsigned i = 1; i < captured.buttons.size(); ++i) {
            const auto& entry = captured.buttons[i];
            assert(!entry.includeNameInUniqueId);
            assert(entry.availabilityTopicSuffix && entry.availabilityTemplate);
            const auto& sensor = captured.binary[i];
            assert(!sensor.includeNameInUniqueId);
            assert(sensor.attributesTemplate);
            SpiRamJsonDocument document(4096);
            HADiscoveryJson::button(document.to<JsonObject>(), "flow/device/cmd", entry.payloadPress);
            document["name"] = entry.name;
            HADiscoveryJson::availability(document.as<JsonObject>(), "flow/device/status",
                                           entry.availabilityTopicSuffix, entry.availabilityTemplate);
            char output[2048]{};
            assert(HADiscoveryJson::serialize(document, output, sizeof(output)));
            SpiRamJsonDocument parsed(4096);
            assert(!deserializeJson(parsed, output));
            assert(std::string(parsed["name"]) == entry.name);
            assert(parsed["ret"] == false);
            assert(parsed["avty"].size() == 2);
            assert(parsed["avty_mode"] == "all");
            SpiRamJsonDocument command(512);
            assert(!deserializeJson(command, parsed["pl_prs"].as<const char*>()));
            assert(command["cmd"] == "alarms.reset");
            unsigned id = command["args"]["id"];
            assert(id == (i == 1 ? 1398U : 1389U));
            assert(std::string(sensor.stateTopicSuffix) == "rt/alarms/id" + std::to_string(id));
            char tooSmall[8]{};
            assert(!HADiscoveryJson::serialize(document, tooSmall, sizeof(tooSmall)));
            document.clear();
            HADiscoveryJson::binarySensor(document.to<JsonObject>(), sensor.stateTopicSuffix,
                                          sensor.valueTemplate, sensor.attributesTemplate);
            assert(HADiscoveryJson::serialize(document, output, sizeof(output)));
            assert(!deserializeJson(parsed, output));
            assert(parsed["json_attr_t"] == sensor.stateTopicSuffix);
            assert(parsed["json_attr_tpl"] == sensor.attributesTemplate);
        }
        module.registerHaEntities_(services); // successful registration is idempotent
        assert(captured.buttons.size() == 3);
    }
    {
        Captured captured;
        captured.rejectBinary = true;
        AlarmModule module;
        module.haSvc_ = &captured.service;
        module.registerHaEntities_(services);
        assert(!module.haEntitiesRegistered_);
        assert(captured.removals.empty()); // preserve old discovery on registration failure
    }
    {
        AlarmModule module;
        auto& slot = module.slots_[3];
        slot.used = true;
        slot.id = AlarmId::PoolPsiLow;
        // Check every combination, including unknown condition.
        for (bool active : {false, true}) for (bool latch : {false, true}) {
            for (auto condition : {AlarmCondState::False, AlarmCondState::True, AlarmCondState::Unknown}) {
                slot.active = active;
                slot.def.latched = latch;
                slot.lastCond = condition;
                bool allowed = active && latch && condition == AlarmCondState::False;
                char output[256];
                assert(module.buildAlarmState_(slot.id, output, sizeof(output)));
                SpiRamJsonDocument state(512);
                assert(!deserializeJson(state, output));
                assert(state["r"].as<bool>() == allowed);
                assert(state["l"].as<bool>() == latch);
                assert(state["c"].as<unsigned>() == static_cast<unsigned>(condition));
                assert(module.resetWithActor_(slot.id, Actor{}) == allowed);
                assert(slot.active == (active && !allowed));
            }
        }
        assert(module.events == 2 && module.activities == 1);
        assert(!module.resetWithActor_(AlarmId::None, Actor{}));
        for (unsigned i = 0; i < 4; ++i) {
            auto& s = module.slots_[i];
            s.used = true; s.active = true; s.id = static_cast<AlarmId>(1000 + i);
            s.def.latched = i != 3;
            s.lastCond = static_cast<AlarmCondState>(i % 3);
        }
        assert(module.resetAllWithActor_(Actor{}) == 1);
        assert(!module.slots_[0].active);
        checkMqttState(module);
        for (unsigned i = 1; i < 4; ++i) assert(module.slots_[i].active);
    }
    failAllocation = true;
    {
        SpiRamJsonDocument empty(4096);
        char output[100];
        assert(!HADiscoveryJson::serialize(empty, output, sizeof(output)));
    }
    failAllocation = false;
    assert(liveAllocations == 0);
}
'''


class AlarmDiscoveryTest(unittest.TestCase):
    def test_existing_button_commands_are_raw_json(self):
        # Changing the serializer must preserve every existing button command.
        count = 0
        for relative in ('PoolDeviceModule/PoolDeviceLifecycle.cpp',
                         'PoolLogicModule/PoolLogicLifecycle.cpp'):
            source = (ROOT / 'src/Modules' / relative).read_text()
            for block in re.findall(r'const HAButtonEntry\s+\w+\{.*?\n\s*\};', source, re.S):
                literals = re.findall(r'"(?:\\.|[^"\\])*"', block)
                payload = next(json.loads(value) for value in literals if 'cmd' in value)
                command = json.loads(payload)
                self.assertIsInstance(command['cmd'], str)
                count += 1
        self.assertEqual(count, 9)

    def test_discovery_roundtrip_and_reset_rules(self):
        header = (ROOT / 'src/Modules/AlarmModule/AlarmModule.h').read_text()
        source = (ROOT / 'src/Modules/AlarmModule/AlarmModule.cpp').read_text()
        structs = '\n'.join(re.search(r'    struct ' + name + r' \{.*?\n    \};', header, re.S)[0]
                            for name in ('AlarmSlot', 'HaAlarmDiscovery'))
        legacy = re.search(r'static constexpr HADiscoveryRemovalEntry kLegacyAlarmButtons\[\].*?\n};', source, re.S)[0]
        methods = '\n'.join(function(source, 'AlarmModule::' + name + '(') for name in (
            'registerHaEntities_', 'findSlotById_', 'buildAlarmState_',
            'resetWithActor_', 'resetAllWithActor_', 'condStateStr_'))
        mqtt_source = (ROOT / 'src/Modules/Network/MQTTModule/MQTTProducers.cpp').read_text()
        mqtt_method = function(mqtt_source, 'MQTTModule::buildAlarm_(')
        ha_source = (ROOT / 'src/Modules/Network/HAModule/HAModule.cpp').read_text()
        ha_methods = '\n'.join(function(ha_source, 'HAModule::' + name + '(') for name in (
            'buildMessage_', 'buildRemovalMessage_', 'publishDiscovery', 'messageCount_'))
        program = (PREAMBLE + legacy + '\nclass AlarmModule { public:\n' + structs + CLASS + methods
                   + MQTT_RUNTIME + mqtt_method + REMOVAL_RUNTIME + ha_methods + MAIN)
        with tempfile.TemporaryDirectory(prefix='flow-ha-alarms-') as directory:
            work = Path(directory)
            (work / 'esp_heap_caps.h').write_text(HEAP_HEADER)
            (work / 'test.cpp').write_text(program)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(work), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'include'),
                            '-I', str(ARDUINO_JSON), str(work / 'test.cpp'), '-o', str(work / 'test')], check=True)
            subprocess.run([str(work / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()

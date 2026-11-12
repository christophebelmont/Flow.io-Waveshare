#pragma once

#include <ArduinoJson.h>
#include <stddef.h>

// Inputs are ordinary strings, never pre-escaped JSON fragments.
namespace HADiscoveryJson {
inline constexpr const char* DisplayRangeAttributesTemplate =
    "{{ ({'min': value_json.min, 'max': value_json.max} | to_json) "
    "if value_json.min is defined and value_json.max is defined else '{}' }}";

inline void availability(JsonObject root, const char* statusTopic,
                         const char* stateTopic = nullptr,
                         const char* stateTemplate = nullptr)
{
    JsonArray topics = root.createNestedArray("avty");
    if (statusTopic && statusTopic[0]) {
        JsonObject status = topics.createNestedObject();
        status["t"] = statusTopic;
        status["val_tpl"] = "{{ 'online' if value_json.online else 'offline' }}";
    }
    if (stateTopic && stateTopic[0] && stateTemplate && stateTemplate[0]) {
        JsonObject state = topics.createNestedObject();
        state["t"] = stateTopic;
        state["val_tpl"] = stateTemplate;
    }
    root["avty_mode"] = "all";
    root["pl_avail"] = "online";
    root["pl_not_avail"] = "offline";
}

inline void button(JsonObject root, const char* commandTopic, const char* payload)
{
    root["cmd_t"] = commandTopic;
    root["pl_prs"] = payload;
    root["ret"] = false; // A reset must never be replayed on reconnect.
}

inline void stateAttributes(JsonObject root, const char* stateTopic, const char* attributesTemplate)
{
    if (attributesTemplate && attributesTemplate[0]) {
        root["json_attr_t"] = stateTopic;
        root["json_attr_tpl"] = attributesTemplate;
    }
}

inline void sensor(JsonObject root, const char* stateTopic, const char* valueTemplate,
                   bool isText, const char* attributesTemplate = nullptr)
{
    root["stat_t"] = stateTopic;
    root["val_tpl"] = valueTemplate;
    if (!isText) root["stat_cla"] = "measurement";
    stateAttributes(root, stateTopic, attributesTemplate);
}

inline void binarySensor(JsonObject root, const char* stateTopic,
                         const char* valueTemplate, const char* attributesTemplate)
{
    root["stat_t"] = stateTopic;
    root["val_tpl"] = valueTemplate;
    root["pl_on"] = "True";
    root["pl_off"] = "False";
    stateAttributes(root, stateTopic, attributesTemplate);
}

inline void mqttSwitch(JsonObject root, const char* stateTopic, const char* valueTemplate,
                       const char* commandTopic, const char* payloadOn, const char* payloadOff,
                       const char* attributesTemplate)
{
    root["stat_t"] = stateTopic;
    root["val_tpl"] = valueTemplate;
    root["stat_on"] = "ON";
    root["stat_off"] = "OFF";
    root["cmd_t"] = commandTopic;
    root["pl_on"] = payloadOn;
    root["pl_off"] = payloadOff;
    stateAttributes(root, stateTopic, attributesTemplate);
}

inline bool serialize(const JsonDocument& doc, char* out, size_t capacity)
{
    if (!out || doc.overflowed() || doc.capacity() == 0) return false;
    const size_t size = measureJson(doc);
    if (size >= capacity) return false;
    return serializeJson(doc, out, capacity) == size;
}
}

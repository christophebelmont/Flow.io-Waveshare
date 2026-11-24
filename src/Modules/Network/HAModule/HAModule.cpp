#include <esp_heap_caps.h>
#include <algorithm>
/**
 * @file HAModule.cpp
 * @brief Implementation file.
 */

#include "HAModule.h"
#include "Core/Values/DataValueAvailability.h"
#include "HADiscoveryJson.h"
#include "Core/SpiRamJsonDocument.h"
#include "Core/BufferUsageTracker.h"
#include "Core/FirmwareVersion.h"
#include "Modules/Network/HAModule/HARuntime.h"
#include "Core/MqttTopics.h"
#include "Core/SystemLimits.h"
#include <ArduinoJson.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <ctype.h>
#include <math.h>
#include <new>
#include <stdarg.h>
#include <string.h>

#ifndef FLOW_HA_BOOT_TRACE
#define FLOW_HA_BOOT_TRACE 0
#endif

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::HAModule)
#include "Core/ModuleLog.h"

#if FLOW_HA_BOOT_TRACE
#define HA_BOOT_TRACE_D(...) LOGD(__VA_ARGS__)
#define HA_BOOT_TRACE_I(...) LOGI(__VA_ARGS__)
#else
#define HA_BOOT_TRACE_D(...) do {} while (0)
#define HA_BOOT_TRACE_I(...) do {} while (0)
#endif

namespace {
static constexpr uint8_t kHaCfgBranch = 1;
static constexpr MqttConfigRouteProducer::Route kHaCfgRoutes[] = {
    {1, {(uint8_t)ConfigModuleId::Ha, kHaCfgBranch}, "ha", "ha", (uint8_t)MqttPublishPriority::Normal, nullptr},
};
static constexpr const char* kHaDeviceConfigUrl = "http://flowio.local";
}

static void buildAvailabilityField(const MqttService* mqttSvc_,
                                   char* out,
                                   size_t outLen,
                                   const char* stateTopic = nullptr,
                                   const char* stateAvailabilityTemplate = nullptr)
{
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!mqttSvc_ || !mqttSvc_->formatTopic) return;

    char availabilityTopic[192] = {0};
    mqttSvc_->formatTopic(mqttSvc_->ctx, MqttTopics::SuffixStatus, availabilityTopic, sizeof(availabilityTopic));
    const bool hasStateAvailability =
        stateTopic && stateTopic[0] != '\0' &&
        stateAvailabilityTemplate && stateAvailabilityTemplate[0] != '\0';

    if (availabilityTopic[0] == '\0') {
        if (!hasStateAvailability) return;
        snprintf(
            out,
            outLen,
            ",\"avty\":[{\"t\":\"%s\",\"val_tpl\":\"%s\"}],"
            "\"avty_mode\":\"all\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"",
            stateTopic,
            stateAvailabilityTemplate
        );
        return;
    }

    if (hasStateAvailability) {
        snprintf(
            out,
            outLen,
            ",\"avty\":[{\"t\":\"%s\",\"val_tpl\":\"%s\"},"
            "{\"t\":\"%s\",\"val_tpl\":\"{{ 'online' if value_json.online else 'offline' }}\"}],"
            "\"avty_mode\":\"all\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"",
            stateTopic,
            stateAvailabilityTemplate,
            availabilityTopic
        );
        return;
    }

    snprintf(
        out,
        outLen,
        ",\"avty\":[{\"t\":\"%s\",\"val_tpl\":\"{{ 'online' if value_json.online else 'offline' }}\"}],"
        "\"avty_mode\":\"all\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"",
        availabilityTopic
    );
}

static bool formatChecked(char* out, size_t outLen, const char* fmt, ...)
{
    if (!out || outLen == 0 || !fmt) return false;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out, outLen, fmt, ap);
    va_end(ap);
    return (n >= 0) && ((size_t)n < outLen);
}

void HAModule::makeDeviceId(char* out, size_t len)
{
    if (!out || len == 0) return;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, "ESP32-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void HAModule::makeHexNodeId(char* out, size_t len)
{
    if (!out || len == 0) return;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, "0x%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void HAModule::sanitizeId(const char* in, char* out, size_t outLen)
{
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t w = 0;
    for (size_t i = 0; in[i] != '\0' && w + 1 < outLen; ++i) {
        char c = in[i];
        if (isalnum((unsigned char)c)) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            out[w++] = c;
        } else {
            out[w++] = '_';
        }
    }
    out[w] = '\0';
}

bool HAModule::addSensorSvc_(const HASensorEntry* entry)
{
    if (!entry) return false;
    return addSensorEntry(*entry);
}

bool HAModule::addBinarySensorSvc_(const HABinarySensorEntry* entry)
{
    if (!entry) return false;
    return addBinarySensorEntry(*entry);
}

bool HAModule::addSwitchSvc_(const HASwitchEntry* entry)
{
    if (!entry) return false;
    return addSwitchEntry(*entry);
}

bool HAModule::addNumberSvc_(const HANumberEntry* entry)
{
    if (!entry) return false;
    return addNumberEntry(*entry);
}

bool HAModule::addSelectSvc_(const HASelectEntry* entry)
{
    if (!entry) return false;
    return addSelectEntry(*entry);
}

bool HAModule::addButtonSvc_(const HAButtonEntry* entry)
{
    if (!entry) return false;
    return addButtonEntry(*entry);
}

bool HAModule::requestRefreshSvc_()
{
    // One-shot resources have already been freed: the Web handler can offer
    // a controlled reboot to rebuild and republish the complete catalogue.
    if (oneShotCompleted_.load() || !cfgData_.enabled || !producerRegistered_) return false;
    requestAutoconfigRefresh();
    return true;
}

static bool isIntegralHaNumberValue(float value)
{
    return fabsf(value - roundf(value)) <= 0.0005f;
}

static bool buildHaNumberRangeField(char* out, size_t outLen, float minValue, float maxValue, float step)
{
    if (!out || outLen == 0) return false;
    if (isIntegralHaNumberValue(minValue) &&
        isIntegralHaNumberValue(maxValue) &&
        isIntegralHaNumberValue(step)) {
        return formatChecked(
            out,
            outLen,
            "\"min\":%ld,\"max\":%ld,\"step\":%ld",
            (long)lroundf(minValue),
            (long)lroundf(maxValue),
            (long)lroundf(step)
        );
    }
    return formatChecked(
        out,
        outLen,
        "\"min\":%.3f,\"max\":%.3f,\"step\":%.3f",
        (double)minValue,
        (double)maxValue,
        (double)step
    );
}

namespace {
template<typename T>
bool reserveDiscoveryEntries(T*& entries, uint16_t& capacity, uint16_t required, uint16_t limit) {
    if (required <= capacity) return true;
    if (required > limit) return false;
    const uint16_t next = std::min<uint16_t>(limit, ((required + 15) / 16) * 16);
    auto* memory = static_cast<T*>(heap_caps_realloc(entries, sizeof(T) * next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!memory) return false;
    for (uint16_t i = capacity; i < next; ++i) new (&memory[i]) T{};
    entries = memory; capacity = next;
    return true;
}
}
bool HAModule::ensureStorage_()
{
    if (oneShotResourcesReleased_) return false;
    if (!pendingBits_) {
        // Two small bitmaps: ready for this pass, deferred for the next.
        // Both live in temporary PSRAM and are released together.
        pendingBits_ = static_cast<uint32_t*>(heap_caps_calloc(
            HA_PENDING_WORDS * 2U, sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (pendingBits_) deferredBits_ = pendingBits_ + HA_PENDING_WORDS;
    }
    return pendingBits_ != nullptr && deferredBits_ != nullptr;
}

size_t HAModule::entityTableUsedBytes_() const
{
    return (size_t)sensorCount_ * sizeof(HASensorEntry) +
           (size_t)binarySensorCount_ * sizeof(HABinarySensorEntry) +
           (size_t)switchCount_ * sizeof(HASwitchEntry) +
           (size_t)numberCount_ * sizeof(HANumberEntry) +
           (size_t)buttonCount_ * sizeof(HAButtonEntry) +
           (size_t)selectCount_ * sizeof(HASelectEntry);
}

size_t HAModule::entityTableCapacityBytes_() const
{
    return (size_t)sensorCapacity_ * sizeof(HASensorEntry) +
           (size_t)binarySensorCapacity_ * sizeof(HABinarySensorEntry) +
           (size_t)switchCapacity_ * sizeof(HASwitchEntry) +
           (size_t)numberCapacity_ * sizeof(HANumberEntry) +
           (size_t)buttonCapacity_ * sizeof(HAButtonEntry) +
           (size_t)selectCapacity_ * sizeof(HASelectEntry);
}

void HAModule::releaseOneShotResources_()
{
#if FLOW_HA_ONESHOT_DISCOVERY
    const uint16_t releasedEntityCount = entityCount_();
    const size_t releasedEntityUsedBytes = entityTableUsedBytes_();
    const size_t releasedEntityCapBytes = entityTableCapacityBytes_();
    const size_t releasedPendingBytes = sizeof(uint32_t) * HA_PENDING_WORDS * 2U;
    heap_caps_free(sensors_);
    heap_caps_free(binarySensors_);
    heap_caps_free(switches_);
    heap_caps_free(numbers_);
    heap_caps_free(buttons_);
    heap_caps_free(selects_);
    heap_caps_free(pendingBits_);
    sensors_ = nullptr;
    binarySensors_ = nullptr;
    switches_ = nullptr;
    numbers_ = nullptr;
    buttons_ = nullptr;
    selects_ = nullptr;
    pendingBits_ = nullptr;
    deferredBits_ = nullptr;
    sensorCount_ = 0;
    binarySensorCount_ = 0;
    switchCount_ = 0;
    numberCount_ = 0;
    buttonCount_ = 0;
    selectCount_ = 0;
    startupReady_ = false;
    published_ = true;
    oneShotCompleted_ = true;
    oneShotResourcesReleased_ = true;
    LOGI("HA PSRAM released: entities=%u table=%u bits=%u free=%lu",
         (unsigned)releasedEntityCount, (unsigned)releasedEntityCapBytes,
         (unsigned)releasedPendingBytes,
         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    HA_BOOT_TRACE_I("ha boot trace: oneshot release entities=%u used=%u cap=%u pending_bytes=%u",
                    (unsigned)releasedEntityCount,
                    (unsigned)releasedEntityUsedBytes,
                    (unsigned)releasedEntityCapBytes,
                    (unsigned)releasedPendingBytes);
#endif
}

bool HAModule::addSensorEntry(const HASensorEntry& entry)
{
    if (oneShotCompleted_ || !ensureStorage_()) return false;
    if (!entry.ownerId || !entry.objectSuffix || !entry.name || !entry.stateTopicSuffix || !entry.valueTemplate) {
        return false;
    }

    for (uint16_t i = 0; i < sensorCount_; ++i) {
        if (strcmp(sensors_[i].ownerId, entry.ownerId) == 0 &&
            strcmp(sensors_[i].objectSuffix, entry.objectSuffix) == 0) {
            sensors_[i] = entry;
            requestAutoconfigRefresh();
            return true;
        }
    }

    if (!reserveDiscoveryEntries(sensors_, sensorCapacity_, sensorCount_ + 1, MAX_HA_SENSORS)) return false;
    sensors_[sensorCount_++] = entry;
    BufferUsageTracker::note(TrackedBufferId::HaEntityTables,
                             entityTableUsedBytes_(),
                             entityTableCapacityBytes_(),
                             entry.objectSuffix,
                             nullptr);
    requestAutoconfigRefresh();
    return true;
}

bool HAModule::addBinarySensorEntry(const HABinarySensorEntry& entry)
{
    if (oneShotCompleted_ || !ensureStorage_()) return false;
    if (!entry.ownerId || !entry.objectSuffix || !entry.name || !entry.stateTopicSuffix || !entry.valueTemplate) {
        return false;
    }

    for (uint16_t i = 0; i < binarySensorCount_; ++i) {
        if (strcmp(binarySensors_[i].ownerId, entry.ownerId) == 0 &&
            strcmp(binarySensors_[i].objectSuffix, entry.objectSuffix) == 0) {
            binarySensors_[i] = entry;
            requestAutoconfigRefresh();
            return true;
        }
    }

    if (!reserveDiscoveryEntries(binarySensors_, binarySensorCapacity_, binarySensorCount_ + 1, MAX_HA_BINARY_SENSORS)) return false;
    binarySensors_[binarySensorCount_++] = entry;
    BufferUsageTracker::note(TrackedBufferId::HaEntityTables,
                             entityTableUsedBytes_(),
                             entityTableCapacityBytes_(),
                             entry.objectSuffix,
                             nullptr);
    requestAutoconfigRefresh();
    return true;
}

bool HAModule::addSwitchEntry(const HASwitchEntry& entry)
{
    if (oneShotCompleted_ || !ensureStorage_()) return false;
    if (!entry.ownerId || !entry.objectSuffix || !entry.name || !entry.stateTopicSuffix ||
        !entry.valueTemplate || !entry.commandTopicSuffix || !entry.payloadOn || !entry.payloadOff) {
        return false;
    }

    for (uint16_t i = 0; i < switchCount_; ++i) {
        if (strcmp(switches_[i].ownerId, entry.ownerId) == 0 &&
            strcmp(switches_[i].objectSuffix, entry.objectSuffix) == 0) {
            switches_[i] = entry;
            requestAutoconfigRefresh();
            return true;
        }
    }

    if (!reserveDiscoveryEntries(switches_, switchCapacity_, switchCount_ + 1, MAX_HA_SWITCHES)) return false;
    switches_[switchCount_++] = entry;
    BufferUsageTracker::note(TrackedBufferId::HaEntityTables,
                             entityTableUsedBytes_(),
                             entityTableCapacityBytes_(),
                             entry.objectSuffix,
                             nullptr);
    requestAutoconfigRefresh();
    return true;
}

bool HAModule::addNumberEntry(const HANumberEntry& entry)
{
    if (oneShotCompleted_ || !ensureStorage_()) return false;
    if (!entry.ownerId || !entry.objectSuffix || !entry.name || !entry.stateTopicSuffix ||
        !entry.valueTemplate || !entry.commandTopicSuffix || !entry.commandTemplate) {
        return false;
    }

    for (uint16_t i = 0; i < numberCount_; ++i) {
        if (strcmp(numbers_[i].ownerId, entry.ownerId) == 0 &&
            strcmp(numbers_[i].objectSuffix, entry.objectSuffix) == 0) {
            numbers_[i] = entry;
            requestAutoconfigRefresh();
            return true;
        }
    }

    if (!reserveDiscoveryEntries(numbers_, numberCapacity_, numberCount_ + 1, MAX_HA_NUMBERS)) {
        LOGW("HA number storage unavailable (%u/%u) reject=%s/%s",
             (unsigned)numberCount_,
             (unsigned)MAX_HA_NUMBERS,
             entry.ownerId ? entry.ownerId : "?",
             entry.objectSuffix ? entry.objectSuffix : "?");
        return false;
    }
    numbers_[numberCount_++] = entry;
    BufferUsageTracker::note(TrackedBufferId::HaEntityTables,
                             entityTableUsedBytes_(),
                             entityTableCapacityBytes_(),
                             entry.objectSuffix,
                             nullptr);
    requestAutoconfigRefresh();
    return true;
}

bool HAModule::addButtonEntry(const HAButtonEntry& entry)
{
    if (oneShotCompleted_ || !ensureStorage_()) return false;
    if (!entry.ownerId || !entry.objectSuffix || !entry.name || !entry.commandTopicSuffix || !entry.payloadPress) {
        return false;
    }

    for (uint16_t i = 0; i < buttonCount_; ++i) {
        if (strcmp(buttons_[i].ownerId, entry.ownerId) == 0 &&
            strcmp(buttons_[i].objectSuffix, entry.objectSuffix) == 0) {
            buttons_[i] = entry;
            requestAutoconfigRefresh();
            return true;
        }
    }

    if (!reserveDiscoveryEntries(buttons_, buttonCapacity_, buttonCount_ + 1, MAX_HA_BUTTONS)) {
        LOGW("HA button storage unavailable (%u/%u) reject=%s/%s",
             (unsigned)buttonCount_,
             (unsigned)MAX_HA_BUTTONS,
             entry.ownerId ? entry.ownerId : "?",
             entry.objectSuffix ? entry.objectSuffix : "?");
        return false;
    }
    buttons_[buttonCount_++] = entry;
    BufferUsageTracker::note(TrackedBufferId::HaEntityTables,
                             entityTableUsedBytes_(),
                             entityTableCapacityBytes_(),
                             entry.objectSuffix,
                             nullptr);
    requestAutoconfigRefresh();
    return true;
}

bool HAModule::addSelectEntry(const HASelectEntry& entry)
{
    if (oneShotCompleted_ || !ensureStorage_()) return false;
    if (!entry.ownerId || !entry.objectSuffix || !entry.name || !entry.stateTopicSuffix ||
        !entry.valueTemplate || !entry.commandTopicSuffix || !entry.commandTemplate ||
        !entry.optionsJson) {
        return false;
    }

    for (uint16_t i = 0; i < selectCount_; ++i) {
        if (strcmp(selects_[i].ownerId, entry.ownerId) == 0 &&
            strcmp(selects_[i].objectSuffix, entry.objectSuffix) == 0) {
            selects_[i] = entry;
            requestAutoconfigRefresh();
            return true;
        }
    }

    if (!reserveDiscoveryEntries(selects_, selectCapacity_, selectCount_ + 1, MAX_HA_SELECTS)) {
        LOGW("HA select storage unavailable (%u/%u) reject=%s/%s",
             (unsigned)selectCount_,
             (unsigned)MAX_HA_SELECTS,
             entry.ownerId ? entry.ownerId : "?",
             entry.objectSuffix ? entry.objectSuffix : "?");
        return false;
    }
    selects_[selectCount_++] = entry;
    BufferUsageTracker::note(TrackedBufferId::HaEntityTables,
                             entityTableUsedBytes_(),
                             entityTableCapacityBytes_(),
                             entry.objectSuffix,
                             nullptr);
    requestAutoconfigRefresh();
    return true;
}

bool HAModule::buildObjectId(const char* suffix, char* out, size_t outLen) const
{
    if (!suffix || !out || outLen == 0) return false;
    char cleanPrefix[sizeof(cfgData_.entityPrefix)] = {0};
    sanitizeId(cfgData_.entityPrefix, cleanPrefix, sizeof(cleanPrefix));
    char cleanSuffix[96] = {0};
    sanitizeId(suffix, cleanSuffix, sizeof(cleanSuffix));
    if (cleanSuffix[0] == '\0') return false;

    size_t prefixLen = strnlen(cleanPrefix, sizeof(cleanPrefix));
    while (prefixLen > 0 && cleanPrefix[prefixLen - 1] == '_') {
        cleanPrefix[--prefixLen] = '\0';
    }

    char raw[256] = {0};
    if (cleanPrefix[0] != '\0') {
        snprintf(raw, sizeof(raw), "%s_%s", cleanPrefix, cleanSuffix);
    } else {
        snprintf(raw, sizeof(raw), "%s", cleanSuffix);
    }
    sanitizeId(raw, out, outLen);
    return out[0] != '\0';
}

bool HAModule::buildDefaultEntityId(const char* component, const char* objectId, char* out, size_t outLen) const
{
    if (!component || !objectId || !out || outLen == 0) return false;
    snprintf(out, outLen, "%s.%s", component, objectId);
    return out[0] != '\0';
}

bool HAModule::buildUniqueId(const char* objectId, const char* name, char* out, size_t outLen) const
{
    if (!objectId || !out || outLen == 0) return false;
    char cleanName[96] = {0};
    sanitizeId(name ? name : "", cleanName, sizeof(cleanName));
    if (cleanName[0] != '\0') {
        snprintf(out, outLen, "%s_%s_%s", deviceId_, objectId, cleanName);
    } else {
        snprintf(out, outLen, "%s_%s", deviceId_, objectId);
    }
    return out[0] != '\0';
}

bool HAModule::publishDiscovery(const char* component, const char* objectId, MqttBuildContext& outCtx)
{
    if (!component || !objectId) return false;
    if (!outCtx.topic || outCtx.topicCapacity == 0 || !outCtx.payload || outCtx.payloadCapacity == 0) return false;
    if (outCtx.payload[0] == '\0') return false;
    if (!formatChecked(outCtx.topic,
                       outCtx.topicCapacity,
                       "%s/%s/%s/%s/config",
                       cfgData_.discoveryPrefix,
                       component,
                       nodeTopicId_,
                       objectId)) {
        LOGW("HA discovery topic truncated component=%s object=%s", component, objectId);
        return false;
    }
    outCtx.topicLen = (uint16_t)strnlen(outCtx.topic, outCtx.topicCapacity);
    outCtx.payloadLen = (uint16_t)strnlen(outCtx.payload, outCtx.payloadCapacity);
    outCtx.qos = 0;
    outCtx.retain = true;
    return true;
}

bool HAModule::publishSensor(const char* objectId, const char* name,
                             const char* stateTopic, const char* valueTemplate,
                             const char* entityCategory, const char* icon, const char* unit,
                             bool hasEntityName,
                             const char* availabilityTemplate,
                             bool isText,
                             MqttBuildContext* outCtx,
                             const char* attributesTemplate, const char* identityName)
{
    if (!outCtx || !stateTopic || !valueTemplate) return false;
    (void)hasEntityName; // Preserve existing sensor naming and unique IDs.
    SpiRamJsonDocument doc(4096);
    if (!initDiscoveryDocument_(doc, "sensor", objectId, identityName ? identityName : name, true)) return false;
    doc["name"] = name;
    if (unit && unit[0]) doc["unit_of_meas"] = unit;
    if (entityCategory && entityCategory[0]) doc["ent_cat"] = entityCategory;
    if (icon && icon[0]) doc["ic"] = icon;
    HADiscoveryJson::sensor(doc.as<JsonObject>(), stateTopic, valueTemplate, isText, attributesTemplate);
    char statusTopic[192]{};
    mqttSvc_->formatTopic(mqttSvc_->ctx, MqttTopics::SuffixStatus, statusTopic, sizeof(statusTopic));
    HADiscoveryJson::availability(doc.as<JsonObject>(), statusTopic, stateTopic, availabilityTemplate);
    if (!HADiscoveryJson::serialize(doc, outCtx->payload, outCtx->payloadCapacity)) {
        LOGW("HA sensor payload overflow object=%s", objectId);
        return false;
    }
    return publishDiscovery("sensor", objectId, *outCtx);
}

bool HAModule::initDiscoveryDocument_(JsonDocument& doc, const char* component,
                                      const char* objectId, const char* name, bool includeNameInUniqueId)
{
    if (!objectId || !name || doc.capacity() == 0) return false;
    char defaultEntityId[224]{};
    char uniqueId[256]{};
    if (!buildDefaultEntityId(component, objectId, defaultEntityId, sizeof(defaultEntityId)) ||
        !buildUniqueId(objectId, includeNameInUniqueId ? name : nullptr, uniqueId, sizeof(uniqueId))) return false;
    doc["name"] = name;
    doc["obj_id"] = objectId;
    // Copy local buffers into the document's PSRAM pool.
    doc["def_ent_id"] = defaultEntityId;
    doc["uniq_id"] = uniqueId;
    doc["has_entity_name"] = false;
    doc["o"]["name"] = originName_;
    JsonObject device = doc.createNestedObject("dev");
    device.createNestedArray("ids").add(deviceIdent_);
    device["name"] = deviceName_;
    device["mf"] = cfgData_.vendor;
    device["mdl"] = cfgData_.model;
    device["sw"] = FirmwareVersion::Full;
    device["cu"] = kHaDeviceConfigUrl;
    return !doc.overflowed();
}

bool HAModule::publishBinarySensor(const char* objectId, const char* name,
                                   const char* stateTopic, const char* valueTemplate,
                                   const char* deviceClass, const char* entityCategory,
                                   const char* icon, const char* attributesTemplate, bool includeNameInUniqueId,
                                   MqttBuildContext* outCtx, const char* availabilityTemplate)
{
    if (!outCtx || !stateTopic || !valueTemplate) return false;
    SpiRamJsonDocument doc(4096);
    if (!initDiscoveryDocument_(doc, "binary_sensor", objectId, name, includeNameInUniqueId)) return false;
    if (deviceClass && deviceClass[0]) doc["dev_cla"] = deviceClass;
    if (entityCategory && entityCategory[0]) doc["ent_cat"] = entityCategory;
    if (icon && icon[0]) doc["ic"] = icon;
    char statusTopic[192]{};
    mqttSvc_->formatTopic(mqttSvc_->ctx, MqttTopics::SuffixStatus, statusTopic, sizeof(statusTopic));
    HADiscoveryJson::availability(doc.as<JsonObject>(), statusTopic,
                                  availabilityTemplate ? stateTopic : nullptr, availabilityTemplate);
    HADiscoveryJson::binarySensor(doc.as<JsonObject>(), stateTopic, valueTemplate, attributesTemplate);
    if (!HADiscoveryJson::serialize(doc, outCtx->payload, outCtx->payloadCapacity)) {
        LOGW("HA binary_sensor payload overflow object=%s", objectId);
        return false;
    }
    return publishDiscovery("binary_sensor", objectId, *outCtx);
}

bool HAModule::publishSwitch(const char* objectId, const HASwitchEntry& entry,
                             const char* stateTopic, const char* commandTopic, MqttBuildContext& outCtx)
{
    SpiRamJsonDocument doc(4096);
    if (!initDiscoveryDocument_(doc, "switch", objectId, entry.name, true)) return false;
    if (entry.entityCategory && entry.entityCategory[0]) doc["ent_cat"] = entry.entityCategory;
    if (entry.icon && entry.icon[0]) doc["ic"] = entry.icon;
    const auto root = doc.as<JsonObject>();
    HADiscoveryJson::mqttSwitch(root, stateTopic, entry.valueTemplate, commandTopic,
                                entry.payloadOn, entry.payloadOff, entry.attributesTemplate);
    char statusTopic[192]{};
    mqttSvc_->formatTopic(mqttSvc_->ctx, MqttTopics::SuffixStatus, statusTopic, sizeof(statusTopic));
    HADiscoveryJson::availability(root, statusTopic);
    if (!HADiscoveryJson::serialize(doc, outCtx.payload, outCtx.payloadCapacity)) {
        LOGW("HA switch payload overflow object=%s", objectId);
        return false;
    }
    return publishDiscovery("switch", objectId, outCtx);
}

bool HAModule::publishNumber(const char* objectId, const char* name,
                             const char* stateTopic, const char* valueTemplate,
                             const char* commandTopic, const char* commandTemplate,
                             float minValue, float maxValue, float step,
                             const char* mode, const char* entityCategory, const char* icon, const char* unit,
                             MqttBuildContext* outCtx)
{
    if (!outCtx) return false;
    MqttBuildContext& buildCtx = *outCtx;
    if (!objectId || !name || !stateTopic || !valueTemplate || !commandTopic || !commandTemplate) return false;
    char defaultEntityId[224] = {0};
    if (!buildDefaultEntityId("number", objectId, defaultEntityId, sizeof(defaultEntityId))) return false;
    char uniqueId[256] = {0};
    if (!buildUniqueId(objectId, name, uniqueId, sizeof(uniqueId))) return false;
    char availabilityField[384] = {0};
    buildAvailabilityField(mqttSvc_, availabilityField, sizeof(availabilityField));

    char unitField[48] = {0};
    if (unit && unit[0] != '\0') {
        snprintf(unitField, sizeof(unitField), ",\"unit_of_meas\":\"%s\"", unit);
    }
    char entityCategoryField[48] = {0};
    if (entityCategory && entityCategory[0] != '\0') {
        snprintf(entityCategoryField, sizeof(entityCategoryField), ",\"ent_cat\":\"%s\"", entityCategory);
    }
    char rangeField[80] = {0};
    if (!buildHaNumberRangeField(rangeField, sizeof(rangeField), minValue, maxValue, step)) {
        LOGW("HA number range payload truncated object=%s", objectId);
        return false;
    }

    if (icon && icon[0] != '\0') {
        if (!formatChecked(buildCtx.payload, buildCtx.payloadCapacity,
                 "{\"name\":\"%s\",\"obj_id\":\"%s\",\"def_ent_id\":\"%s\",\"uniq_id\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"%s\",\"cmd_t\":\"%s\",\"cmd_tpl\":\"%s\","
                 "%s,\"mode\":\"%s\",\"ic\":\"%s\"%s%s%s,"
                 "\"o\":{\"name\":\"%s\"},"
                 "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\","
                 "\"mf\":\"%s\",\"mdl\":\"%s\",\"sw\":\"%s\",\"cu\":\"%s\"}}",
                 name, objectId, defaultEntityId, uniqueId, stateTopic, valueTemplate, commandTopic, commandTemplate,
                 rangeField, mode ? mode : "slider", icon, entityCategoryField, unitField, availabilityField,
                 originName_, deviceIdent_, deviceName_, cfgData_.vendor, cfgData_.model, FirmwareVersion::Full, kHaDeviceConfigUrl)) {
            LOGW("HA number payload truncated object=%s", objectId);
            return false;
        }
    } else {
        if (!formatChecked(buildCtx.payload, buildCtx.payloadCapacity,
                 "{\"name\":\"%s\",\"obj_id\":\"%s\",\"def_ent_id\":\"%s\",\"uniq_id\":\"%s\",\"stat_t\":\"%s\","
                 "\"val_tpl\":\"%s\",\"cmd_t\":\"%s\",\"cmd_tpl\":\"%s\","
                 "%s,\"mode\":\"%s\"%s%s%s,"
                 "\"o\":{\"name\":\"%s\"},"
                 "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\","
                 "\"mf\":\"%s\",\"mdl\":\"%s\",\"sw\":\"%s\",\"cu\":\"%s\"}}",
                 name, objectId, defaultEntityId, uniqueId, stateTopic, valueTemplate, commandTopic, commandTemplate,
                 rangeField, mode ? mode : "slider", entityCategoryField, unitField, availabilityField,
                 originName_, deviceIdent_, deviceName_, cfgData_.vendor, cfgData_.model, FirmwareVersion::Full, kHaDeviceConfigUrl)) {
            LOGW("HA number payload truncated object=%s", objectId);
            return false;
        }
    }
    return publishDiscovery("number", objectId, buildCtx);
}

bool HAModule::publishSelect(const char* objectId, const char* name,
                             const char* stateTopic, const char* valueTemplate,
                             const char* commandTopic, const char* commandTemplate,
                             const char* optionsJson,
                             const char* icon,
                             const char* entityCategory,
                             MqttBuildContext* outCtx)
{
    if (!outCtx) return false;
    MqttBuildContext& buildCtx = *outCtx;
    if (!objectId || !name || !stateTopic || !valueTemplate || !commandTopic ||
        !commandTemplate || !optionsJson) {
        return false;
    }
    char defaultEntityId[224] = {0};
    if (!buildDefaultEntityId("select", objectId, defaultEntityId, sizeof(defaultEntityId))) return false;
    char uniqueId[256] = {0};
    if (!buildUniqueId(objectId, name, uniqueId, sizeof(uniqueId))) return false;
    char availabilityField[384] = {0};
    buildAvailabilityField(mqttSvc_, availabilityField, sizeof(availabilityField));
    char entityCategoryField[48] = {0};
    if (entityCategory && entityCategory[0] != '\0') {
        snprintf(entityCategoryField, sizeof(entityCategoryField), ",\"ent_cat\":\"%s\"", entityCategory);
    }

    if (icon && icon[0] != '\0') {
        if (!formatChecked(buildCtx.payload, buildCtx.payloadCapacity,
                 "{\"name\":\"%s\",\"obj_id\":\"%s\",\"def_ent_id\":\"%s\",\"uniq_id\":\"%s\","
                 "\"stat_t\":\"%s\",\"val_tpl\":\"%s\",\"cmd_t\":\"%s\",\"cmd_tpl\":\"%s\","
                 "\"options\":%s,\"ic\":\"%s\"%s%s,"
                 "\"o\":{\"name\":\"%s\"},"
                 "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\","
                 "\"mf\":\"%s\",\"mdl\":\"%s\",\"sw\":\"%s\",\"cu\":\"%s\"}}",
                 name, objectId, defaultEntityId, uniqueId,
                 stateTopic, valueTemplate, commandTopic, commandTemplate,
                 optionsJson, icon, entityCategoryField, availabilityField,
                 originName_, deviceIdent_, deviceName_, cfgData_.vendor, cfgData_.model, FirmwareVersion::Full, kHaDeviceConfigUrl)) {
            LOGW("HA select payload truncated object=%s", objectId);
            return false;
        }
    } else {
        if (!formatChecked(buildCtx.payload, buildCtx.payloadCapacity,
                 "{\"name\":\"%s\",\"obj_id\":\"%s\",\"def_ent_id\":\"%s\",\"uniq_id\":\"%s\","
                 "\"stat_t\":\"%s\",\"val_tpl\":\"%s\",\"cmd_t\":\"%s\",\"cmd_tpl\":\"%s\","
                 "\"options\":%s%s%s,"
                 "\"o\":{\"name\":\"%s\"},"
                 "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\","
                 "\"mf\":\"%s\",\"mdl\":\"%s\",\"sw\":\"%s\",\"cu\":\"%s\"}}",
                 name, objectId, defaultEntityId, uniqueId,
                 stateTopic, valueTemplate, commandTopic, commandTemplate,
                 optionsJson, entityCategoryField, availabilityField,
                 originName_, deviceIdent_, deviceName_, cfgData_.vendor, cfgData_.model, FirmwareVersion::Full, kHaDeviceConfigUrl)) {
            LOGW("HA select payload truncated object=%s", objectId);
            return false;
        }
    }

    return publishDiscovery("select", objectId, buildCtx);
}

bool HAModule::publishButton(const char* objectId, const char* name,
                             const char* commandTopic, const char* payloadPress,
                             const char* entityCategory, const char* icon,
                             const char* availabilityTopic, const char* availabilityTemplate, bool includeNameInUniqueId,
                             MqttBuildContext* outCtx)
{
    if (!outCtx || !commandTopic || !payloadPress) return false;
    SpiRamJsonDocument doc(4096);
    if (!initDiscoveryDocument_(doc, "button", objectId, name, includeNameInUniqueId)) return false;
    if (entityCategory && entityCategory[0]) doc["ent_cat"] = entityCategory;
    if (icon && icon[0]) doc["ic"] = icon;
    char statusTopic[192]{};
    mqttSvc_->formatTopic(mqttSvc_->ctx, MqttTopics::SuffixStatus, statusTopic, sizeof(statusTopic));
    HADiscoveryJson::availability(doc.as<JsonObject>(), statusTopic, availabilityTopic, availabilityTemplate);
    HADiscoveryJson::button(doc.as<JsonObject>(), commandTopic, payloadPress);
    if (!HADiscoveryJson::serialize(doc, outCtx->payload, outCtx->payloadCapacity)) {
        LOGW("HA button payload overflow object=%s", objectId);
        return false;
    }
    return publishDiscovery("button", objectId, *outCtx);
}

void HAModule::setStartupReady(bool ready)
{
    startupReady_.store(ready);
}

void HAModule::setBranding(const char* objectPrefix,
                           const char* originName,
                           const char* vendor,
                           const char* model)
{
    if (objectPrefix && objectPrefix[0] != '\0') {
        snprintf(objectPrefix_, sizeof(objectPrefix_), "%s", objectPrefix);
        sanitizeId(objectPrefix_, objectPrefix_, sizeof(objectPrefix_));
        if (objectPrefix_[0] == '\0') snprintf(objectPrefix_, sizeof(objectPrefix_), "fio");
        snprintf(cfgData_.entityPrefix, sizeof(cfgData_.entityPrefix), "%s", objectPrefix_);
    }
    if (originName && originName[0] != '\0') {
        snprintf(originName_, sizeof(originName_), "%s", originName);
    }
    if (vendor && vendor[0] != '\0') {
        snprintf(cfgData_.vendor, sizeof(cfgData_.vendor), "%s", vendor);
    }
    if (model && model[0] != '\0') {
        snprintf(cfgData_.model, sizeof(cfgData_.model), "%s", model);
    }
}

void HAModule::refreshIdentityFromConfig()
{
    if (cfgData_.deviceId[0] != '\0') {
        strncpy(deviceId_, cfgData_.deviceId, sizeof(deviceId_) - 1);
        deviceId_[sizeof(deviceId_) - 1] = '\0';
    } else {
        makeHexNodeId(deviceId_, sizeof(deviceId_));
    }
    sanitizeId(deviceId_, nodeTopicId_, sizeof(nodeTopicId_));
    if (nodeTopicId_[0] == '\0') {
        snprintf(nodeTopicId_, sizeof(nodeTopicId_), "flowio");
    }

    char mqttTopicDeviceId[64] = {0};
    const char* idForEntityPrefix = deviceId_;
    if (resolveMqttTopicDeviceId_(mqttTopicDeviceId, sizeof(mqttTopicDeviceId))) {
        idForEntityPrefix = mqttTopicDeviceId;
    }

    // HA `device.identifiers` should follow the effective MQTT topic device id when available.
    snprintf(deviceIdent_, sizeof(deviceIdent_), "%s-%s", cfgData_.vendor, idForEntityPrefix);
    refreshDeviceNameFromMqttConfig_();
}

void HAModule::refreshDeviceNameFromMqttConfig_()
{
    snprintf(deviceName_, sizeof(deviceName_), "%s", originName_);
    if (!cfgSvc_ || !cfgSvc_->toJsonModule) return;

    // Keep the HA producer stack small. Both the temporary MQTT config
    // string and the JSON pool are allocated in PSRAM, never on the task stack.
    char* mqttJson = static_cast<char*>(heap_caps_calloc(
        Limits::Mqtt::Buffers::StateCfg, sizeof(char), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!mqttJson) return;
    bool truncated = false;
    if (cfgSvc_->toJsonModule(cfgSvc_->ctx, "mqtt", mqttJson,
                              Limits::Mqtt::Buffers::StateCfg, &truncated) && !truncated) {
        SpiRamJsonDocument doc(512);
        const DeserializationError err = deserializeJson(doc, mqttJson);
        if (!err && doc.is<JsonObjectConst>()) {
            const char* configuredName = doc["deviceName"] | "";
            if (configuredName && configuredName[0] != '\0') {
                snprintf(deviceName_, sizeof(deviceName_), "%s", configuredName);
            }
        }
    }
    heap_caps_free(mqttJson);
}

bool HAModule::resolveMqttTopicDeviceId_(char* out, size_t outLen) const
{
    if (!out || outLen == 0U) return false;
    out[0] = '\0';
    if (!mqttSvc_ || !mqttSvc_->formatTopic) return false;

    char topic[192] = {0};
    mqttSvc_->formatTopic(mqttSvc_->ctx, MqttTopics::SuffixStatus, topic, sizeof(topic));
    if (topic[0] == '\0') return false;

    // Topic format: <base>/<deviceId>/status. Keep deviceId extraction robust when base contains slashes.
    char* lastSlash = strrchr(topic, '/');
    if (!lastSlash || lastSlash == topic) return false;
    *lastSlash = '\0'; // Strip trailing "/status"

    char* deviceSlash = strrchr(topic, '/');
    if (!deviceSlash || *(deviceSlash + 1) == '\0') return false;

    const int n = snprintf(out, outLen, "%s", deviceSlash + 1);
    return n > 0 && (size_t)n < outLen;
}

uint16_t HAModule::entityCount_() const
{
    return (uint16_t)sensorCount_ +
           (uint16_t)binarySensorCount_ +
           (uint16_t)switchCount_ +
           (uint16_t)numberCount_ +
           (uint16_t)selectCount_ +
           (uint16_t)buttonCount_;
}

bool HAModule::isPending_(uint16_t messageId) const
{
    if (!pendingBits_) return false;
    if (messageId >= MAX_HA_ENTITIES) return false;
    const uint16_t word = (uint16_t)(messageId / 32U);
    const uint16_t bit = (uint16_t)(messageId % 32U);
    return (pendingBits_[word] & (1UL << bit)) != 0UL;
}

void HAModule::setPending_(uint16_t messageId, bool pending)
{
    if (!pendingBits_) return;
    if (messageId >= MAX_HA_ENTITIES) return;
    const uint16_t word = (uint16_t)(messageId / 32U);
    const uint16_t bit = (uint16_t)(messageId % 32U);
    const uint32_t mask = (1UL << bit);
    if (pending) pendingBits_[word] |= mask;
    else pendingBits_[word] &= ~mask;
}

void HAModule::markAllPending_()
{
    if (oneShotCompleted_ || !ensureStorage_()) return;
    memset(pendingBits_, 0, sizeof(uint32_t) * HA_PENDING_WORDS * 2U);
    deferredBuildId_ = UINT16_MAX;
    deferredRetryNotBeforeMs_ = 0U;
    retryPass_ = 0U;
    discoveredCount_ = 0U;
    retrySkippedCount_ = 0U;
    const uint16_t count = entityCount_();
    for (uint16_t i = 0; i < count; ++i) {
        setPending_(i, true);
    }
    HA_BOOT_TRACE_D("ha boot trace: mark all pending entities=%u", (unsigned)count);
}

bool HAModule::anyReadyPending_() const
{
    if (!pendingBits_) return false;
    for (uint16_t i = 0; i < HA_PENDING_WORDS; ++i) {
        if (pendingBits_[i] != 0U) return true;
    }
    return false;
}

bool HAModule::anyPending_() const
{
    if (!pendingBits_ || !deferredBits_) return false;
    for (uint16_t i = 0; i < HA_PENDING_WORDS; ++i) {
        if ((pendingBits_[i] | deferredBits_[i]) != 0U) return true;
    }
    return false;
}

MqttBuildResult HAModule::deferEntityBuild_(uint16_t messageId)
{
    // The MQTT job can be released immediately. Its drop callback moves
    // this entity to a temporary bitmap for a later bounded retry pass.
    deferredBuildId_ = messageId;
    return MqttBuildResult::NoLongerNeeded;
}

bool HAModule::enqueuePending_(MqttPublishPriority prio)
{
    if (oneShotCompleted_) return false;
    if (!mqttSvc_ || !mqttSvc_->tryEnqueueBackground) return false;
    if (!discoveryReady_() || admittedMessageId_ != UINT16_MAX) return false;

    if (!anyReadyPending_() && anyPending_()) {
        const uint32_t nowMs = millis();
        if (deferredRetryNotBeforeMs_ == 0U) {
            deferredRetryNotBeforeMs_ = nowMs + DiscoveryRetryDelayMs;
            LOGI("HA deferred retry pending pass=%u delay_ms=%lu",
                 (unsigned)(retryPass_ + 1U), (unsigned long)DiscoveryRetryDelayMs);
        }
        if ((int32_t)(nowMs - deferredRetryNotBeforeMs_) < 0) return false;
        deferredRetryNotBeforeMs_ = 0U;
        if (retryPass_ >= DiscoveryRetryPasses) {
            uint16_t unfinished = 0U;
            for (uint16_t word = 0U; word < HA_PENDING_WORDS; ++word) {
                unfinished += (uint16_t)__builtin_popcount((unsigned)deferredBits_[word]);
            }
            LOGW("HA discovery INCOMPLETE after retries: published=%u missing=%u deferred_attempts=%u passes=%u; reboot/refresh to retry",
                 (unsigned)discoveredCount_, (unsigned)unfinished,
                 (unsigned)retrySkippedCount_, (unsigned)retryPass_);
            memset(deferredBits_, 0, sizeof(uint32_t) * HA_PENDING_WORDS);
            // Mark the one-shot attempt terminal even when some entities
            // could not be built. The Waveshare discovery heap is released
            // via haAutoconfigPublished(); leaving it false would leak PSRAM.
            published_ = true;
            if (dsSvc_ && dsSvc_->store) setHaAutoconfigPublished(*dsSvc_->store, true);
            return false;
        }
        ++retryPass_;
        for (uint16_t word = 0; word < HA_PENDING_WORDS; ++word) {
            pendingBits_[word] = deferredBits_[word];
            deferredBits_[word] = 0U;
        }
        LOGI("HA discovery retry pass=%u/%u published=%u",
             (unsigned)retryPass_, (unsigned)DiscoveryRetryPasses,
             (unsigned)discoveredCount_);
    }

    const uint16_t count = entityCount_();
    for (uint16_t i = 0; i < count; ++i) {
        if (!isPending_(i)) continue;
        const auto result = mqttSvc_->tryEnqueueBackground(mqttSvc_->ctx, ProducerId, i, prio);
        if (result == MqttAdmissionResult::Invalid) {
            LOGE("Invalid HA publication message=%u", (unsigned)i);
            onMessageDropped_(i);
            return false;
        }
        const bool queued = result == MqttAdmissionResult::Accepted;
        if (queued) admittedMessageId_ = i;
        HA_BOOT_TRACE_D("ha boot trace: enqueue message=%u/%u prio=%u queued=%d",
                        (unsigned)i,
                        (unsigned)count,
                        (unsigned)prio,
                        queued ? 1 : 0);
        return queued;
    }
    HA_BOOT_TRACE_D("ha boot trace: enqueue skipped (no pending)");
    return false;
}

bool HAModule::discoveryReady_() const
{
    if (!startupReady_) return false;
    if (!dsSvc_ || !dsSvc_->store) return false;
    const DataStore& ds = *dsSvc_->store;
    return networkReady(ds) && mqttReady(ds);
}

MqttBuildResult HAModule::producerBuildStatic_(void* ctx, uint16_t messageId, MqttBuildContext& buildCtx)
{
    HAModule* self = static_cast<HAModule*>(ctx);
    return self ? self->buildMessage_(messageId, buildCtx) : MqttBuildResult::PermanentError;
}

void HAModule::producerPublishedStatic_(void* ctx, uint16_t messageId)
{
    HAModule* self = static_cast<HAModule*>(ctx);
    if (self) self->onMessagePublished_(messageId);
}

void HAModule::producerDroppedStatic_(void* ctx, uint16_t messageId)
{
    HAModule* self = static_cast<HAModule*>(ctx);
    if (self) self->onMessageDropped_(messageId);
}

bool HAModule::buildEntityMessage_(uint16_t messageId, MqttBuildContext& buildCtx)
{
    // Registration allocates each nonempty table. Unused entity types have no storage.
    bool ok = false;
    uint16_t cursor = 0;

    if (messageId < (uint16_t)(cursor + sensorCount_)) {
        const HASensorEntry& e = sensors_[messageId - cursor];
        if (buildObjectId(e.objectSuffix, objectIdBuf_, sizeof(objectIdBuf_))) {
            mqttSvc_->formatTopic(mqttSvc_->ctx, e.stateTopicSuffix, stateTopicBuf_, sizeof(stateTopicBuf_));
            ok = publishSensor(objectIdBuf_, e.name, stateTopicBuf_, e.valueTemplate,
                               e.entityCategory, e.icon, e.unit, e.hasEntityName, e.availabilityTemplate, e.isText, &buildCtx, e.attributesTemplate, e.identityName);
        }
    } else {
        cursor = (uint16_t)(cursor + sensorCount_);
        if (messageId < (uint16_t)(cursor + binarySensorCount_)) {
            const HABinarySensorEntry& e = binarySensors_[messageId - cursor];
            if (buildObjectId(e.objectSuffix, objectIdBuf_, sizeof(objectIdBuf_))) {
                mqttSvc_->formatTopic(mqttSvc_->ctx, e.stateTopicSuffix, stateTopicBuf_, sizeof(stateTopicBuf_));
                ok = publishBinarySensor(objectIdBuf_, e.name, stateTopicBuf_, e.valueTemplate, e.deviceClass, e.entityCategory, e.icon, e.attributesTemplate, e.includeNameInUniqueId, &buildCtx, e.availabilityTemplate);
            }
        } else {
            cursor = (uint16_t)(cursor + binarySensorCount_);
            if (messageId < (uint16_t)(cursor + switchCount_)) {
                const HASwitchEntry& e = switches_[messageId - cursor];
                if (buildObjectId(e.objectSuffix, objectIdBuf_, sizeof(objectIdBuf_))) {
                    mqttSvc_->formatTopic(mqttSvc_->ctx, e.stateTopicSuffix, stateTopicBuf_, sizeof(stateTopicBuf_));
                    mqttSvc_->formatTopic(mqttSvc_->ctx, e.commandTopicSuffix, commandTopicBuf_, sizeof(commandTopicBuf_));
                    ok = publishSwitch(objectIdBuf_, e, stateTopicBuf_, commandTopicBuf_, buildCtx);
                }
            } else {
                cursor = (uint16_t)(cursor + switchCount_);
                if (messageId < (uint16_t)(cursor + numberCount_)) {
                    const HANumberEntry& e = numbers_[messageId - cursor];
                    if (buildObjectId(e.objectSuffix, objectIdBuf_, sizeof(objectIdBuf_))) {
                        mqttSvc_->formatTopic(mqttSvc_->ctx, e.stateTopicSuffix, stateTopicBuf_, sizeof(stateTopicBuf_));
                        mqttSvc_->formatTopic(mqttSvc_->ctx, e.commandTopicSuffix, commandTopicBuf_, sizeof(commandTopicBuf_));
                        ok = publishNumber(objectIdBuf_, e.name, stateTopicBuf_, e.valueTemplate,
                                           commandTopicBuf_, e.commandTemplate,
                                           e.minValue, e.maxValue, e.step,
                                           e.mode, e.entityCategory, e.icon, e.unit, &buildCtx);
                    }
                } else {
                    cursor = (uint16_t)(cursor + numberCount_);
                    if (messageId < (uint16_t)(cursor + selectCount_)) {
                        const HASelectEntry& e = selects_[messageId - cursor];
                        if (buildObjectId(e.objectSuffix, objectIdBuf_, sizeof(objectIdBuf_))) {
                            mqttSvc_->formatTopic(mqttSvc_->ctx, e.stateTopicSuffix, stateTopicBuf_, sizeof(stateTopicBuf_));
                            mqttSvc_->formatTopic(mqttSvc_->ctx, e.commandTopicSuffix, commandTopicBuf_, sizeof(commandTopicBuf_));
                            ok = publishSelect(objectIdBuf_, e.name, stateTopicBuf_, e.valueTemplate,
                                               commandTopicBuf_, e.commandTemplate, e.optionsJson,
                                               e.icon, e.entityCategory, &buildCtx);
                        }
                    } else {
                        cursor = (uint16_t)(cursor + selectCount_);
                        if (messageId < (uint16_t)(cursor + buttonCount_)) {
                            const HAButtonEntry& e = buttons_[messageId - cursor];
                            if (buildObjectId(e.objectSuffix, objectIdBuf_, sizeof(objectIdBuf_))) {
                                mqttSvc_->formatTopic(mqttSvc_->ctx, e.commandTopicSuffix, commandTopicBuf_, sizeof(commandTopicBuf_));
                                stateTopicBuf_[0] = '\0';
                                if (e.availabilityTopicSuffix) {
                                    mqttSvc_->formatTopic(mqttSvc_->ctx, e.availabilityTopicSuffix, stateTopicBuf_, sizeof(stateTopicBuf_));
                                }
                                ok = publishButton(objectIdBuf_, e.name, commandTopicBuf_, e.payloadPress,
                                                   e.entityCategory, e.icon, stateTopicBuf_, e.availabilityTemplate, e.includeNameInUniqueId, &buildCtx);
                            }
                        }
                    }
                }
            }
        }
    }

    if (!ok) return false;
    return true;
}

MqttBuildResult HAModule::buildMessage_(uint16_t messageId, MqttBuildContext& buildCtx)
{
    if (oneShotCompleted_) return MqttBuildResult::NoLongerNeeded;
    if (!startupReady_) return MqttBuildResult::RetryLater;
    if (!cfgData_.enabled) return MqttBuildResult::NoLongerNeeded;
    if (!mqttSvc_ || !mqttSvc_->formatTopic) return MqttBuildResult::PermanentError;
    if (!dsSvc_ || !dsSvc_->store) return MqttBuildResult::RetryLater;
    if (!networkReady(*dsSvc_->store)) return MqttBuildResult::RetryLater;
    if (!mqttReady(*dsSvc_->store)) return MqttBuildResult::RetryLater;
    if (!isPending_(messageId)) return MqttBuildResult::NoLongerNeeded;

    refreshIdentityFromConfig();
    setHaVendor(*dsSvc_->store, cfgData_.vendor);
    setHaDeviceId(*dsSvc_->store, deviceId_);

    const uint16_t entityCount = entityCount_();
    HA_BOOT_TRACE_D("ha boot trace: build message=%u entity_count=%u",
                    (unsigned)messageId,
                    (unsigned)entityCount);
    if (messageId >= entityCount) return MqttBuildResult::NoLongerNeeded;
    // Registration precedes acquisition. Gate catalogue discovery here, after startup,
    // rather than treating every not-yet-sampled value as unavailable at registration.
    const ValueId id = messageId < sensorCount_ ? sensors_[messageId].dataValueId :
        messageId < sensorCount_ + binarySensorCount_ ? binarySensors_[messageId - sensorCount_].dataValueId : VALUE_INVALID;
    if (id != VALUE_INVALID) {
        ValueSnapshot value{}; ValueMetadata metadata{};
        if (!dsSvc_->store->publishHa(id)) return MqttBuildResult::NoLongerNeeded;
        if (!readAvailableDataValue(dsSvc_->store->values, id, value, metadata)) {
            if (retryPass_ < DiscoveryRetryPasses)
                return deferEntityBuild_(messageId);
            // After bounded retries, allow a defined but not-yet-valid sensor
            // to appear as unavailable in HA. Existing availability templates
            // already convey offline quality; do not announce undefined slots.
            const auto* descriptor = dataValueDescriptor(id);
            if (!descriptor ||
                !dsSvc_->store->values.read(id, value, &metadata) ||
                !dataValueCompatible(*descriptor, metadata))
                return deferEntityBuild_(messageId);
            LOGI("HA publishing registered but unavailable value id=%u after %u retries",
                 (unsigned)id, (unsigned)retryPass_);
        }
    }
    if (!buildEntityMessage_(messageId, buildCtx)) {
        // Short-lived PSRAM or formatting pressure should not erase an entity.
        return deferEntityBuild_(messageId);
    }
    return MqttBuildResult::Ready;
}

void HAModule::onMessagePublished_(uint16_t messageId)
{
    admittedMessageId_ = UINT16_MAX;
    deferredBuildId_ = UINT16_MAX;
    ++discoveredCount_;
    setPending_(messageId, false);
    const bool done = !anyPending_();
    if (dsSvc_ && dsSvc_->store) {
        setHaAutoconfigPublished(*dsSvc_->store, done);
    }
    published_ = done;
    HA_BOOT_TRACE_D("ha boot trace: published message=%u done=%d",
                    (unsigned)messageId,
                    done ? 1 : 0);
#if !FLOW_HA_ONESHOT_DISCOVERY
    if (done) markBootLogCaptureCompleteAfterDiscovery_();
#endif
}

void HAModule::onMessageDropped_(uint16_t messageId)
{
    admittedMessageId_ = UINT16_MAX;
    const bool deferred = deferredBuildId_ == messageId;
    deferredBuildId_ = UINT16_MAX;
    setPending_(messageId, false);
    if (deferred && deferredBits_ && messageId < MAX_HA_ENTITIES) {
        deferredBits_[messageId / 32U] |= (1UL << (messageId % 32U));
        ++retrySkippedCount_;
    }
    const bool done = !anyPending_();
    if (dsSvc_ && dsSvc_->store) {
        setHaAutoconfigPublished(*dsSvc_->store, done);
    }
    published_ = done;
    HA_BOOT_TRACE_I("ha boot trace: dropped message=%u done=%d",
                    (unsigned)messageId,
                    done ? 1 : 0);
#if !FLOW_HA_ONESHOT_DISCOVERY
    if (done) markBootLogCaptureCompleteAfterDiscovery_();
#endif
}

void HAModule::markBootLogCaptureCompleteAfterDiscovery_()
{
#if FLOW_ENABLE_BOOT_LOG_CAPTURE
    markBootLogCaptureComplete();
#endif
}

void HAModule::requestAutoconfigRefresh()
{
    if (!oneShotCompleted_) refreshRequested_.store(true);
}

void HAModule::producerTickStatic_(void* ctx, uint32_t)
{
    static_cast<HAModule*>(ctx)->prepareDiscovery_();
}

bool HAModule::producerBackgroundStatic_(void* ctx)
{
    return static_cast<HAModule*>(ctx)->enqueuePending_(MqttPublishPriority::Low);
}

void HAModule::prepareDiscovery_()
{
    if (oneShotCompleted_) return;
    if (refreshRequested_.exchange(false)) {
        published_ = false;
        markAllPending_();
        if (dsSvc_ && dsSvc_->store) setHaAutoconfigPublished(*dsSvc_->store, false);
    }
#if FLOW_HA_ONESHOT_DISCOVERY
    // Nothing to publish is also a completed catalogue. Do this only after
    // startup registration is finished and MQTT is ready, otherwise the
    // earlier init() call (which starts with zero entries) could prematurely
    // free the tables before the other modules register their HA entities.
    if (startupReady_.load() && discoveryReady_() && entityCount_() == 0U &&
        admittedMessageId_ == UINT16_MAX && !anyPending_()) {
        published_ = true;
        if (dsSvc_ && dsSvc_->store)
            setHaAutoconfigPublished(*dsSvc_->store, true);
        LOGI("HA discovery empty catalogue: no entities registered");
    }
    if (published_ && admittedMessageId_ == UINT16_MAX && !anyPending_()) {
        LOGI("HA one-shot discovery complete, releasing resources");
        markBootLogCaptureCompleteAfterDiscovery_();
        // Admission, builders, completion and cleanup share the MQTT task.
        releaseOneShotResources_();
    }
#endif
}

void HAModule::loop()
{
#if FLOW_HA_ONESHOT_DISCOVERY
    if (oneShotCompleted_.load()) {
        finishLoopTask();
        return;
    }
#endif
    delayNextCycle(pdMS_TO_TICKS(250));
}

void HAModule::onConfigLoaded(ConfigStore&, ServiceRegistry& services)
{
#if FLOW_HA_ONESHOT_DISCOVERY
    (void)services;
    return;
#else
    if (!cfgMqttPub_) {
        cfgMqttPub_ = new (std::nothrow) MqttConfigRouteProducer();
    }
    if (cfgMqttPub_) {
        cfgMqttPub_->configure(this,
                               ProducerIdCfg,
                               kHaCfgRoutes,
                               (uint8_t)(sizeof(kHaCfgRoutes) / sizeof(kHaCfgRoutes[0])),
                               services);
    }
#endif
}

void HAModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    constexpr uint8_t kCfgModuleId = (uint8_t)ConfigModuleId::Ha;
    constexpr uint8_t kCfgBranchId = kHaCfgBranch;
    cfg.registerVar(enabledVar, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(vendorVar, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(deviceIdVar, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(entityPrefixVar, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(prefixVar, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(modelVar, kCfgModuleId, kCfgBranchId);
    HA_BOOT_TRACE_I("ha boot trace: init oneshot=%d max_entities=%u",
                    FLOW_HA_ONESHOT_DISCOVERY ? 1 : 0,
                    (unsigned)MAX_HA_ENTITIES);

    dsSvc_ = services.get<DataStoreService>(ServiceId::DataStore);
    cfgSvc_ = services.get<ConfigStoreService>(ServiceId::ConfigStore);
    mqttSvc_ = services.get<MqttService>(ServiceId::Mqtt);
    if (!ensureStorage_()) {
        LOGE("HA storage allocation failed");
    }

    if (!services.add(ServiceId::Ha, &haSvc_)) {
        LOGE("service registration failed: %s", toString(ServiceId::Ha));
    }

    producer_.producerId = ProducerId;
    producer_.ctx = this;
    producer_.buildMessage = HAModule::producerBuildStatic_;
    producer_.onMessagePublished = HAModule::producerPublishedStatic_;
    producer_.onMessageDropped = HAModule::producerDroppedStatic_;
    producer_.onTransportTick = HAModule::producerTickStatic_;
    producer_.onBackgroundTick = HAModule::producerBackgroundStatic_;
    if (mqttSvc_ && mqttSvc_->registerProducer) {
        producerRegistered_ = mqttSvc_->registerProducer(mqttSvc_->ctx, &producer_);
    }

    if (dsSvc_ && dsSvc_->store) {
        setHaAutoconfigPublished(*dsSvc_->store, false);
    }

    markAllPending_();
}

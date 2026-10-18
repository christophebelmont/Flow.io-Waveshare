/**
 * @file DataStoreModule.cpp
 * @brief Implementation file.
 */
#include "DataStoreModule.h"
#include "DataExposureConfig.h"
#include "Core/Values/ValueFormat.h"
#include <new>
#include "Core/Values/InternalAnalogSources.h"
#include "Core/Values/PoolDeviceValues.h"
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::DataStoreModule)
#include "Core/ModuleLog.h"

void DataStoreModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    if (!_store.values.begin()) {
        LOGE("Value storage allocation failed bytes=%u", (unsigned)ValueRegistry::storageBytes());
    } else {
        if (!PoolDeviceValues::define(_store.values)) LOGE("Equipment value catalog initialization failed");
        if (!InternalAnalogSources::define(_store.values)) LOGE("Internal source catalog initialization failed");
        LOGI("Value storage ready bytes=%u memory=%s", (unsigned)ValueRegistry::storageBytes(),
             _store.values.storageInPsram() ? "psram" : "internal");
    }
    void* memory = heap_caps_malloc(sizeof(DataExposureConfig), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory) { exposure_ = new (memory) DataExposureConfig{}; exposure_->registerConfig(cfg, _store); }
    else LOGE("Data exposure configuration allocation failed");
    auto* eb = services.get<EventBusService>(ServiceId::EventBus);
    if (eb && eb->bus) {
        _store.setEventBus(eb->bus);
    }

    if (!services.add(ServiceId::DataStore, &_svc)) {
        LOGE("service registration failed: %s", toString(ServiceId::DataStore));
    }
}

void DataStoreModule::onConfigLoaded(ConfigStore&, ServiceRegistry&) {
    if (exposure_) exposure_->apply(_store);
}
const DataValueDescriptor* DataStoreModule::route_(uint8_t idx) const {
    for (const auto& d : kDataValues) {
        // Physical inputs and derived values retain their existing MQTT topics/providers.
        if (d.kind == DataValueKind::Analog || d.kind == DataValueKind::Derived || !_store.publishHa(d.id)) continue;
        if (idx-- == 0) return &d;
    }
    return nullptr;
}
uint8_t DataStoreModule::runtimeSnapshotCount() const {
    uint8_t count = 0;
    while (route_(count)) ++count;
    return count;
}
const char* DataStoreModule::runtimeSnapshotSuffix(uint8_t idx) const {
    const auto* d = route_(idx); return d ? d->topic : nullptr;
}
bool DataStoreModule::runtimeSnapshotAffectsKey(uint8_t idx, DataKey key) const {
    const auto* d = route_(idx); return d && key == DataKeys::ValueBase + d->id;
}
bool DataStoreModule::buildRuntimeSnapshot(uint8_t idx, char* out, size_t len, uint32_t& stamp) const {
    const auto* d = route_(idx);
    ValueSnapshot value{}; ValueMetadata meta{};
    if (!d || !_store.values.read(d->id, value, &meta)) return false;
    if (!dataValueCompatible(*d, meta)) value.quality = ValueQuality::Invalid;
    stamp = value.sequence ? value.sequence : 1;
    return formatValueSnapshot(out, len, d->id, value, meta);
}

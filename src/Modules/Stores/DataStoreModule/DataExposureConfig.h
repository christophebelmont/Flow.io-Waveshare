#pragma once
#include "Core/ConfigStore.h"
#include "Core/EventBus/EventPayloads.h"
#include "Core/Generated/DataValueCatalog_Generated.h"
#include "Core/DataStore/DataStore.h"
#include <stdio.h>

/** Persistent policy only. Measurement storage remains in DataStore::values. */
struct DataExposureConfig {
    static_assert(ValueIds::Equipment + ValueIds::PoolTankSlots <= UINT8_MAX, "Exposure branch range");
    struct Entry {
        bool ha = false, history = true, usable = true;
        char path[20]{}, haKey[12]{}, historyKey[12]{}, usableKey[12]{};
        ConfigVariable<bool, 0> haVar{}, historyVar{}, usableVar{};
        ValueId id = VALUE_INVALID;
        Entry* next = nullptr;
    };
    Entry* first = nullptr;
    ConfigStore* config = nullptr;
    DataStore* store = nullptr;
    ~DataExposureConfig() {
        while (first) { auto* entry = first; first = first->next; entry->~Entry(); heap_caps_free(entry); }
    }
    bool add(ValueId id, bool load = true) {
        for (auto* entry = first; entry; entry = entry->next) if (entry->id == id) return true;
        const auto* descriptor = dataValueDescriptor(id);
        if (!descriptor || !config || !store || !config->canRegister(3)) return false;
        void* memory = heap_caps_malloc(sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!memory) return false;
        auto& e = *new (memory) Entry{};
        const auto& d = *descriptor;
        auto& cfg = *config;
        e.id = id;
        e.ha = d.defaultHa;
        e.history = !dataValueIsEquipment(d);
        if (!store->bindExposureConfig(d.id, &e.ha, &e.history, &e.usable)) { e.~Entry(); heap_caps_free(memory); return false; }
        snprintf(e.path, sizeof(e.path), "data/value/d%03u", d.id);
        snprintf(e.haKey, sizeof(e.haKey), "dv%03uha", d.id);
        snprintf(e.historyKey, sizeof(e.historyKey), "dv%03uhs", d.id);
        snprintf(e.usableKey, sizeof(e.usableKey), "dv%03uus", d.id);
        e.usableVar = {e.usableKey, "usable", e.path, ConfigType::Bool, &e.usable, ConfigPersistence::Persistent, 0};
        e.haVar = {e.haKey, "publish_ha", e.path, ConfigType::Bool, &e.ha, ConfigPersistence::Persistent, 0};
        e.historyVar = {e.historyKey, "history", e.path, ConfigType::Bool, &e.history, ConfigPersistence::Persistent, 0};
        // Equipment values share one notification branch per equipment; storage keys remain per value.
        const uint8_t branch = dataValueIsEquipment(d) ? uint8_t(ValueIds::Equipment + 1 + d.slot) : uint8_t(d.id + 1);
        cfg.registerVar(e.usableVar, uint8_t(ConfigModuleId::DataStore), branch);
        cfg.registerVar(e.haVar, uint8_t(ConfigModuleId::DataStore), branch);
        cfg.registerVar(e.historyVar, uint8_t(ConfigModuleId::DataStore), branch);
        e.next = first; first = &e;
        if (load) {
            cfg.loadPersistentVar(e.usableVar); cfg.loadPersistentVar(e.haVar); cfg.loadPersistentVar(e.historyVar);
            applyEntry(*store, e);
        }
        return true;
    }
    static void applyEntry(DataStore& store, const Entry& e) {
        store.values.setUsable(e.id, e.usable);
        store.setPublishHa(e.id, e.ha);
        store.values.setHistory(e.id, e.history);
    }
    void registerConfig(ConfigStore& cfg, DataStore& dataStore) {
        config = &cfg; store = &dataStore;
        for (const auto& d : kDataValues) if (!dataValueIsEquipment(d)) add(d.id, false);
        store->setExposureRegistrar([](void* context, ValueId id) {
            return static_cast<DataExposureConfig*>(context)->add(id);
        }, this);
    }
    void apply(DataStore& dataStore) const {
        for (auto* entry = first; entry; entry = entry->next) applyEntry(dataStore, *entry);
    }
};

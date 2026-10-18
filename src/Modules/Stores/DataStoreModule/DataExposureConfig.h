#pragma once
#include "Core/ConfigStore.h"
#include "Core/EventBus/EventPayloads.h"
#include "Core/Generated/DataValueCatalog_Generated.h"
#include "Core/DataStore/DataStore.h"
#include <stdio.h>

/** Persistent policy only. Measurement storage remains in DataStore::values. */
struct DataExposureConfig {
    struct Entry {
        bool ha = false, history = true, usable = true;
        char path[20]{}, haKey[12]{}, historyKey[12]{}, usableKey[12]{};
        ConfigVariable<bool, 0> haVar{}, historyVar{}, usableVar{};
    } entries[kDataValueCount];
    void registerConfig(ConfigStore& cfg, DataStore& store) {
        for (size_t i = 0; i < kDataValueCount; ++i) {
            const auto& d = kDataValues[i];
            auto& e = entries[i];
            e.ha = d.defaultHa;
            store.bindExposureConfig(d.id, &e.ha, &e.history, &e.usable);
            snprintf(e.path, sizeof(e.path), "data/value/d%03u", d.id);
            snprintf(e.haKey, sizeof(e.haKey), "dv%03uha", d.id);
            snprintf(e.historyKey, sizeof(e.historyKey), "dv%03uhs", d.id);
            snprintf(e.usableKey, sizeof(e.usableKey), "dv%03uus", d.id);
            e.usableVar = {e.usableKey, "usable", e.path, ConfigType::Bool, &e.usable, ConfigPersistence::Persistent, 0};
            e.haVar = {e.haKey, "publish_ha", e.path, ConfigType::Bool, &e.ha, ConfigPersistence::Persistent, 0};
            e.historyVar = {e.historyKey, "history", e.path, ConfigType::Bool, &e.history, ConfigPersistence::Persistent, 0};
            cfg.registerVar(e.usableVar, uint8_t(ConfigModuleId::DataStore), uint8_t(d.id + 1));
            cfg.registerVar(e.haVar, uint8_t(ConfigModuleId::DataStore), uint8_t(d.id + 1));
            cfg.registerVar(e.historyVar, uint8_t(ConfigModuleId::DataStore), uint8_t(d.id + 1));
        }
    }
    void apply(DataStore& store) const {
        for (size_t i = 0; i < kDataValueCount; ++i) {
            store.values.setUsable(kDataValues[i].id, entries[i].usable);
            store.setPublishHa(kDataValues[i].id, entries[i].ha);
            store.values.setHistory(kDataValues[i].id, entries[i].history);
        }
    }
};

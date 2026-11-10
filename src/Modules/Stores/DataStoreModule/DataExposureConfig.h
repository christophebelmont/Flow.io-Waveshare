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
    static constexpr uint16_t RangePageSize = 64;
    static constexpr uint8_t RangePageCount = (ValueIds::Capacity + RangePageSize - 1) / RangePageSize;
    static_assert(RangePageCount <= 16, "Display range storage key capacity");
    struct RangeRecord {
        uint8_t state = 0; // 0: catalogue default, 1: custom range, 2: explicitly no range
        double min = 0.0;
        double max = 0.0;
    };
    static_assert(sizeof(RangeRecord) == 24, "Unexpected aligned display range record size");
    struct RangePageRecord {
        uint8_t state = 0;
        uint8_t min[sizeof(double)]{};
        uint8_t max[sizeof(double)]{};
    };
    static_assert(sizeof(RangePageRecord) == 1 + 2 * sizeof(double), "Stable display range record layout");
    struct RangePage {
        uint8_t version = 1;
        uint8_t page = 0;
        RangePageRecord records[RangePageSize]{};
    };
    static_assert(sizeof(RangePage) == 2 + RangePageSize * (1 + 2 * sizeof(double)),
                  "Display range NVS page layout must stay version stable");
    struct RangePageBuffer {
        RangePage* value = nullptr;
        RangePageBuffer() {
            void* memory = heap_caps_malloc(sizeof(RangePage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (memory) value = new (memory) RangePage{};
        }
        ~RangePageBuffer() {
            if (value) { value->~RangePage(); heap_caps_free(value); }
        }
    };
    struct RangeLock {
        SemaphoreHandle_t mutex;
        explicit RangeLock(SemaphoreHandle_t value) : mutex(value) { xSemaphoreTake(mutex, portMAX_DELAY); }
        ~RangeLock() { xSemaphoreGive(mutex); }
    };
    RangeRecord displayRanges[ValueIds::Capacity]{};
    StaticSemaphore_t rangeMutexStorage{};
    SemaphoreHandle_t rangeMutex = xSemaphoreCreateMutexStatic(&rangeMutexStorage);
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
        loadDisplayRanges();
        store->values.setDisplayRangeResolver([](void* context, ValueId id, ValueDisplayRange& range) {
            return static_cast<DataExposureConfig*>(context)->resolveDisplayRange(id, range);
        }, this);
        store->setDisplayRangePersister([](void* context, ValueId id, const ValueDisplayRange& range) {
            return static_cast<DataExposureConfig*>(context)->saveDisplayRange(id, range);
        }, this);
        for (const auto& d : kDataValues) if (!dataValueIsEquipment(d)) add(d.id, false);
        store->setExposureRegistrar([](void* context, ValueId id) {
            return static_cast<DataExposureConfig*>(context)->add(id);
        }, this);
    }
    void apply(DataStore& dataStore) const {
        for (auto* entry = first; entry; entry = entry->next) applyEntry(dataStore, *entry);
        for (ValueId id = 0; id < ValueIds::Capacity; ++id) {
            ValueDisplayRange range{};
            if (displayRanges[id].state == 1) range = {true, displayRanges[id].min, displayRanges[id].max};
            else if (displayRanges[id].state == 2) range = {};
            else continue; // Registry owns catalogue defaults.
            dataStore.values.setDisplayRange(id, range);
        }
    }

    void loadDisplayRanges() {
        if (!config) return;
        for (uint8_t pageIndex = 0; pageIndex < RangePageCount; ++pageIndex) {
            RangePageBuffer pageBuffer;
            if (!pageBuffer.value) continue;
            auto& page = *pageBuffer.value;
            char key[8]; snprintf(key, sizeof(key), "dvr%02u", pageIndex);
            size_t actual = 0;
            if (!config->readRuntimeBlob(key, &page, sizeof(page), &actual)) continue;
            if (actual != sizeof(page) || page.version != 1 || page.page != pageIndex) continue;
            const ValueId firstId = ValueId(pageIndex * RangePageSize);
            const ValueId endId = firstId + RangePageSize < ValueIds::Capacity
                ? firstId + RangePageSize : ValueIds::Capacity;
            for (ValueId id = firstId; id < endId; ++id) {
                const auto& record = page.records[id - firstId];
                double min = 0.0, max = 0.0;
                memcpy(&min, record.min, sizeof(min));
                memcpy(&max, record.max, sizeof(max));
                if (record.state > 2 || (record.state == 1 && !validDisplayRange(min, max))) continue;
                displayRanges[id] = {record.state, min, max};
            }
        }
    }

    bool resolveDisplayRange(ValueId id, ValueDisplayRange& range) {
        if (id >= ValueIds::Capacity) return false;
        RangeLock lock(rangeMutex);
        const auto& record = displayRanges[id];
        if (record.state == 1) {
            range = {true, record.min, record.max};
            return true;
        }
        if (record.state == 2) {
            range = {};
            return true;
        }
        return false;
    }

    bool saveDisplayRange(ValueId id, const ValueDisplayRange& range) {
        if (!config || id >= ValueIds::Capacity || (range.hasRange && !validDisplayRange(range.min, range.max))) return false;
        RangeLock lock(rangeMutex);
        const uint8_t pageIndex = id / RangePageSize;
        RangePageBuffer pageBuffer;
        if (!pageBuffer.value) return false;
        auto& page = *pageBuffer.value;
        page.page = pageIndex;
        char key[8]; snprintf(key, sizeof(key), "dvr%02u", pageIndex);
        size_t actual = 0;
        if (config->readRuntimeBlob(key, &page, sizeof(page), &actual)) {
            if (actual != sizeof(page) || page.version != 1 || page.page != pageIndex) page = RangePage{};
        }
        page.version = 1;
        page.page = pageIndex;
        auto& record = page.records[id % RangePageSize];
        const uint8_t storedState = record.state;
        double storedMin = 0.0, storedMax = 0.0;
        memcpy(&storedMin, record.min, sizeof(storedMin));
        memcpy(&storedMax, record.max, sizeof(storedMax));
        const auto defaults = defaultValueDisplayRange(id);
        RangeRecord next{};
        if (range.hasRange == defaults.hasRange &&
            (!range.hasRange || (range.min == defaults.min && range.max == defaults.max))) {
            record = {};
        } else if (range.hasRange) {
            next = {1, range.min, range.max};
            record.state = next.state;
            memcpy(record.min, &next.min, sizeof(next.min));
            memcpy(record.max, &next.max, sizeof(next.max));
        } else {
            record = {};
            record.state = 2;
            next.state = 2;
        }
        if (next.state == storedState &&
            (next.state != 1 || (next.min == storedMin && next.max == storedMax))) {
            displayRanges[id] = next;
            return true;
        }
        if (!config->writeRuntimeBlob(key, &page, sizeof(page))) return false;
        displayRanges[id] = next;
        return true;
    }
};

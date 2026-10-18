#pragma once
/**
 * @file DataStoreModule.h
 * @brief Module that exposes DataStore service.
 */
#include "Core/ModulePassive.h"
#include "Core/RuntimeSnapshotProvider.h"
#include "Core/SystemLimits.h"
struct DataExposureConfig;
#include "Core/DataStore/DataStore.h"
#include "Core/Generated/DataValueCatalog_Generated.h"
#include "Core/Services/Services.h"

/**
 * @brief Passive module wiring the DataStore service.
 */
class DataStoreModule : public ModulePassive, public IRuntimeSnapshotProvider {
public:
    /** @brief Module id. */
    ModuleId moduleId() const override { return ModuleId::DataStore; }

    /** @brief DataStore depends on EventBus. */
    uint8_t dependencyCount() const override { return 1; }
    ModuleId dependency(uint8_t i) const override {
        if (i == 0) return ModuleId::EventBus;
        return ModuleId::Unknown;
    }

    /** @brief Initialize DataStore and register service. */
    void init(ConfigStore& cfg, ServiceRegistry& services) override;

    void onConfigLoaded(ConfigStore&, ServiceRegistry&) override;
    uint8_t runtimeSnapshotCount() const override;
    const char* runtimeSnapshotSuffix(uint8_t idx) const override;
    uint32_t runtimeSnapshotRefreshMs(uint8_t) const override { return Limits::Mqtt::Timing::SensorRefreshMs; }
    RuntimeRouteClass runtimeSnapshotClass(uint8_t) const override { return RuntimeRouteClass::NumericThrottled; }
    bool runtimeSnapshotAffectsKey(uint8_t idx, DataKey key) const override;
    bool buildRuntimeSnapshot(uint8_t idx, char* out, size_t len, uint32_t& maxTsOut) const override;
private:
    const DataValueDescriptor* route_(uint8_t idx) const;
    DataExposureConfig* exposure_ = nullptr;
    DataStore _store;
    DataStoreService _svc{ &_store };
};

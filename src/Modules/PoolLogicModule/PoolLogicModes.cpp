/** @file PoolLogicModes.cpp
 * Single configuration authority, legacy migration and mode transitions.
 */
#include "PoolLogicModule.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::PoolLogicModule)
#include "Core/ModuleLog.h"

bool PoolLogicModule::validateModes_(void* ctx, const ConfigCandidate& candidate)
{
    auto& self = *static_cast<PoolLogicModule*>(ctx);
    uint8_t mode, correction;
    return candidate.read(self.operatingModeVar_, self.operatingMode_, mode) && poolModeValid(mode) &&
           candidate.read(self.phCorrectionTypeVar_, self.phCorrectionType_, correction) && poolPhCorrectionValid(correction);
}

bool PoolLogicModule::migrateModes_()
{
    if (!cfgStore_) return false;
    bool modeExists, correctionExists, migrated;
    if (!cfgStore_->persistentKeyExists(NvsKeys::PoolLogic::OperatingMode, modeExists) ||
        !cfgStore_->persistentKeyExists(NvsKeys::PoolLogic::PhCorrectionType, correctionExists) ||
        !cfgStore_->persistentKeyExists(NvsKeys::PoolLogic::ModesMigrated, migrated)) return false;

    if (!migrated) {
        LegacyPoolModes legacy;
        const auto load = [&](const char* key, bool& value) {
            ConfigVariable<bool, 0> var{key, nullptr, nullptr, ConfigType::Bool,
                &value, ConfigPersistence::Persistent, 0};
            cfgStore_->loadPersistentVar(var);
        };
        load(NvsKeys::PoolLogic::Enabled, legacy.enabled);
        load(NvsKeys::PoolLogic::AutoMode, legacy.automatic);
        load(NvsKeys::PoolLogic::WinterMode, legacy.winter);
        load(NvsKeys::PoolLogic::PhAutoMode, legacy.phAutomatic);
        load(NvsKeys::PoolLogic::PhDosePlus, legacy.phPlus);
        load(NvsKeys::PoolLogic::OrpAutoMode, legacy.disinfectionAutomatic);
        if (!modeExists) operatingMode_ = uint8_t(migratePoolOperatingMode(legacy));
        if (!correctionExists) phCorrectionType_ = uint8_t(migratePoolPhCorrection(legacy));
        // Only the old chlorine/bromine protocol consumed dis_auto_mode.
        if (!modeExists && !legacy.disinfectionAutomatic &&
            isDisinfectionType_(DisinfectionChlorineBromine)) disinfectionType_ = DisinfectionDisabled;
        if (!poolModeValid(operatingMode_)) operatingMode_ = uint8_t(PoolOperatingMode::Manual);
        if (!poolPhCorrectionValid(phCorrectionType_)) phCorrectionType_ = uint8_t(PoolPhCorrection::Disabled);
        if (!cfgStore_->persistRegisteredValue(&disinfectionType_) ||
            !cfgStore_->persistRegisteredValue(&phCorrectionType_) ||
            !cfgStore_->persistRegisteredValue(&operatingMode_)) return false;
        const uint8_t version = 1;
        if (!cfgStore_->writeRuntimeBlob(NvsKeys::PoolLogic::ModesMigrated, &version, sizeof(version))) return false;
    }
    if (!poolModeValid(operatingMode_) || !poolPhCorrectionValid(phCorrectionType_)) {
        // Repair both fields in one validated candidate, including the case
        // where both stored values are invalid.
        char patch[112];
        snprintf(patch, sizeof(patch),
            "{\"poollogic/modes\":{\"operating_mode\":%u},\"poollogic/ph\":{\"correction_type\":%u}}",
            poolModeValid(operatingMode_) ? unsigned(operatingMode_) : unsigned(PoolOperatingMode::Manual),
            poolPhCorrectionValid(phCorrectionType_) ? unsigned(phCorrectionType_) : unsigned(PoolPhCorrection::Disabled));
        if (!cfgStore_->applyJson(patch)) return false;
    }

    // The marker and all replacement values are durable before deleting old keys.
    // This cleanup concerns local NVS only. No HA discovery deletion is published.
    for (const char* key : {NvsKeys::PoolLogic::Enabled, NvsKeys::PoolLogic::AutoMode,
            NvsKeys::PoolLogic::WinterMode, NvsKeys::PoolLogic::PhAutoMode,
            NvsKeys::PoolLogic::OrpAutoMode, NvsKeys::PoolLogic::PhDosePlus}) {
        bool exists;
        if (!cfgStore_->persistentKeyExists(key, exists) || (exists && !cfgStore_->eraseKey(key))) return false;
    }
    return true;
}

bool PoolLogicModule::serviceManualControlAllowed_(void* ctx, uint8_t slot)
{
    const auto& self = *static_cast<PoolLogicModule*>(ctx);
    bool managed = false;
    for (auto roleSlot : {self.filtrationDeviceSlot_, self.phPumpDeviceSlot_, self.orpPumpDeviceSlot_,
            self.robotDeviceSlot_, self.swgDeviceSlot_, self.heaterDeviceSlot_, self.fillingDeviceSlot_})
        managed = managed || slot == roleSlot;
    return !managed || (self.modesReady_ && self.bootControlReady_ &&
        self.appliedMode_ == self.operatingMode_ && self.appliedPhCorrection_ == self.phCorrectionType_ &&
        self.appliedDisinfection_ == self.disinfectionType_ && self.operatingMode() == PoolOperatingMode::Manual);
}

void PoolLogicModule::reconcileModes_(uint32_t nowMs)
{
    if (appliedMode_ == 0xFF) {
        // Boot is not a strategy change. Durable, compatible leases retain their
        // remaining duration; the normal control loop rebuilds guided targets.
        appliedMode_ = operatingMode_;
        appliedPhCorrection_ = phCorrectionType_;
        appliedDisinfection_ = disinfectionType_;
        modeSinceMs_ = nowMs;
        return;
    }
    const bool modeChanged = appliedMode_ != operatingMode_;
    const bool correctionChanged = appliedPhCorrection_ != phCorrectionType_;
    const bool treatmentChanged = appliedDisinfection_ != disinfectionType_;
    if (!modeChanged && !correctionChanged && !treatmentChanged) return;

    bool ready = true;
    // Any change of strategy terminates leases on the affected roles. PoolDevice
    // remains the sole owner of timers, persistence and physical dependencies.
    for (auto slot : {filtrationDeviceSlot_, phPumpDeviceSlot_, orpPumpDeviceSlot_,
            robotDeviceSlot_, swgDeviceSlot_, heaterDeviceSlot_, fillingDeviceSlot_}) {
        const bool affected = modeChanged || (correctionChanged && slot == phPumpDeviceSlot_) ||
            (treatmentChanged && (slot == orpPumpDeviceSlot_ || slot == swgDeviceSlot_));
        if (!affected || slot >= POOL_DEVICE_MAX) continue;
        const auto info = readDeviceControlInfo_(slot);
        if (!info.available) continue;
        if (poolSvc_ && poolSvc_->releaseOverride)
            ready = (poolSvc_->releaseOverride(poolSvc_->ctx, slot, ActuatorOverrideReason::Configuration) == POOLDEV_SVC_OK) && ready;
        // Manual filtration inherits its current guided demand. All other
        // automatic requests are explicitly stopped before the new mode takes over.
        if (slot != filtrationDeviceSlot_ && info.guidedOn) ready = writeDeviceDesired_(slot, false) && ready;
    }
    resetTemporalPidState_(phPidState_, nowMs);
    resetTemporalPidState_(orpPidState_, nowMs);
    phPidEnabled_ = false;
    orpPidEnabled_ = false;
    o2LastProgressMs_ = 0;
    if (!ready) return; // Retry before accepting direct manual commands.
    if (modeChanged) modeSinceMs_ = nowMs;
    appliedMode_ = operatingMode_;
    appliedPhCorrection_ = phCorrectionType_;
    appliedDisinfection_ = disinfectionType_;
    portENTER_CRITICAL(&pendingMux_);
    pendingFiltrationReconcile_ = true;
    portEXIT_CRITICAL(&pendingMux_);
}

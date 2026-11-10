#include "PoolDeviceModule.h"
#include "Core/ConfigStore.h"
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::PoolDeviceModule)
#include "Core/ModuleLog.h"

bool PoolDeviceModule::restoreRunning_(uint8_t slot)
{
    auto& s = slots_[slot];
    // Stable, versioned wire format: no C++ struct padding or driver identity.
    uint8_t record[2]{};
    size_t length = 0;
    s.savedRunningValid = cfgStore_ &&
        cfgStore_->readRuntimeBlob(s.runStateKey, record, sizeof(record), &length) &&
        length == sizeof(record) && record[0] == 1 && record[1] <= 1;
    s.savedRunning = s.savedRunningValid && record[1] != 0;
    // Configuration pages associate dNN and pdNN independently of the
    // driver's physical outputs. Read that policy by logical slot, even when
    // the corresponding IO endpoint is disabled or the driver is serial.
    IoOutputBootPolicy policy{};
    if (!ioSvc_ || !ioSvc_->outputBootPolicy ||
        ioSvc_->outputBootPolicy(ioSvc_->ctx, slot, &policy) != IO_OK) {
        LOGW("PoolDevice slot=%u startup policy unavailable; keeping stopped", slot);
        return false;
    }
    return policy.retainPrevious && s.savedRunningValid ? s.savedRunning : policy.initialOn;
}

bool PoolDeviceModule::persistRunning_(uint8_t slot, bool running)
{
    auto& s = slots_[slot];
    if (s.savedRunningValid && s.savedRunning == running) {
        s.runStateSaveFailed = false;
        return true;
    }
    const uint8_t record[] = {1, uint8_t(running)};
    s.runStateSaveAtMs = millis();
    s.runStateSaveFailed = !cfgStore_ ||
        !cfgStore_->writeRuntimeBlob(s.runStateKey, record, sizeof(record));
    if (s.runStateSaveFailed) {
        LOGW("PoolDevice slot=%u cannot persist running=%u", slot, unsigned(running));
        return false;
    }
    s.savedRunning = running;
    s.savedRunningValid = true;
    return true;
}

bool PoolDeviceModule::bootDependenciesPending_(uint8_t slot, uint32_t now,
                                               bool dependenciesReady, bool unsafe)
{
    auto& s = slots_[slot];
    if (!s.bootResumePending) return false;
    // Only the initial request may wait for upstream equipment to start. After
    // release, the normal interlock cancels demand on any dependency loss.
    if (!s.def.enabled || !s.driverReady || maxUptimeReached_(s) || unsafe ||
        (writesEnabled_ && (dependenciesReady || uint32_t(now - s.bootResumeSinceMs) >= 20000U)))
        s.bootResumePending = false;
    return s.bootResumePending && !dependenciesReady;
}

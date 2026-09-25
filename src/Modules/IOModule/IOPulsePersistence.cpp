#include "IOModule.h"
#include "Core/LogModuleIds.h"
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

bool IOModule::loadPulseCheckpoint_()
{
    static_assert(MAX_DIGITAL_INPUTS <= PulseCheckpoint::Capacity, "Pulse checkpoint capacity");
    static_assert(PulseCheckpoint::Size <= Limits::Config::Capacity::RuntimeBlobAsyncMax, "Pulse blob capacity");
    if (!cfgStore_) return false;
    uint8_t bytes[PulseCheckpoint::Size]{};
    size_t length = 0;
    const bool found = cfgStore_->readRuntimeBlob(PulseCheckpoint::Key, bytes, sizeof(bytes), &length);
    if (found || length != 0) {
        if (!found || !PulseCheckpoint::decode(bytes, length, pulsePersisted_)) return false;
    } else {
        // New format: deliberately discard legacy float totals. Commit migration
        // before any pulse driver is started. No runtime synchronous flash writes.
        pulsePersisted_ = {};
        for (uint8_t i = 0; i < MAX_DIGITAL_INPUTS; ++i)
            pulsePersisted_.resetToken[i] = digitalInCfg_[i].counterReset;
        PulseCheckpoint::encode(pulsePersisted_, bytes);
        if (!cfgStore_->writeRuntimeBlob(PulseCheckpoint::Key, bytes, sizeof(bytes))) return false;
        ++pulseCheckpointWrites_;
    }
    pulseLastAttemptMs_ = millis();
    return true;
}

void IOModule::checkpointPulses_(uint32_t nowMs)
{
    if (!pulseStorageReady_ || !cfgSvc_ || !cfgSvc_->writeRuntimeBlobTracked) return;
    const uint32_t status = pulseReceipt_.status.load();
    if (status == PersistenceReceipt::Pending) return;
    if (status == PersistenceReceipt::Succeeded) {
        pulsePersisted_ = pulsePending_;
        ++pulseCheckpointWrites_;
        pulseRetry_ = false;
        pulseReceipt_.status.store(PersistenceReceipt::Idle);
    } else if (status == PersistenceReceipt::Failed) {
        ++pulseCheckpointFailures_;
        pulseRetry_ = true;
        pulseReceipt_.status.store(PersistenceReceipt::Idle);
        LOGW("Pulse checkpoint failed; retaining last confirmed counts");
    }
    const uint32_t period = pulseRetry_ ? PulseCheckpoint::RetryMs : PulseCheckpoint::PeriodMs;
    if (uint32_t(nowMs - pulseLastAttemptMs_) < period) return;
    pulseLastAttemptMs_ = nowMs;
    pulsePending_ = pulsePersisted_;
    bool dirty = false;
    for (uint8_t i = 0; i < MAX_DIGITAL_SLOTS; ++i) {
        const auto& slot = digitalSlots_[i];
        if (!slot.used || slot.kind != DIGITAL_SLOT_INPUT || slot.inDef.mode != IO_DIGITAL_INPUT_COUNTER ||
            !slot.pulse.initialized || slot.pulse.overflow) continue;
        const auto index = slot.logicalIdx;
        pulsePending_.count[index] = slot.pulse.count;
        pulsePending_.generation[index] = slot.pulse.generation;
        pulsePending_.resetToken[index] = slot.counterResetSeen;
        dirty |= pulsePending_.count[index] != pulsePersisted_.count[index] ||
                 pulsePending_.generation[index] != pulsePersisted_.generation[index] ||
                 pulsePending_.resetToken[index] != pulsePersisted_.resetToken[index];
    }
    if (!dirty) return;
    uint8_t bytes[PulseCheckpoint::Size];
    PulseCheckpoint::encode(pulsePending_, bytes);
    if (!cfgSvc_->writeRuntimeBlobTracked(cfgSvc_->ctx, PulseCheckpoint::Key, bytes, sizeof(bytes), &pulseReceipt_)) {
        ++pulseCheckpointFailures_;
        pulseRetry_ = true;
    }
}

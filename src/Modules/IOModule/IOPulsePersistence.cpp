#include "IOModule.h"
#include "Core/CommandRegistry.h"
#include <ArduinoJson.h>
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

void IOModule::checkpointPulses_(uint32_t nowMs, bool force)
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
    if (!force && uint32_t(nowMs - pulseLastAttemptMs_) < period) return;
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
    } else {
        pulseRetry_ = false;
    }
}

// Only the IO task touches pulse state and the persistence receipt. Callers publish
// a request and wait for its completion; a timed-out request retains its storage.
IoStatus IOModule::requestPulseSave_(IoId resetId)
{
    if (pulseRequestCaller_.test_and_set()) return IO_ERR_BUSY;
    const auto state = pulseRequestState_.load();
    if (state != PulseRequestState::Idle && state != PulseRequestState::Complete) {
        pulseRequestCaller_.clear();
        return IO_ERR_BUSY;
    }
    pulseRequestResetId_ = resetId;
    pulseRequestStartedMs_ = millis();
    pulseRequestState_.store(PulseRequestState::Requested);
    const uint32_t started = millis();
    while (pulseRequestState_.load() != PulseRequestState::Complete &&
           uint32_t(millis() - started) < PulseRequestTimeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const IoStatus result = pulseRequestState_.load() == PulseRequestState::Complete
                                ? pulseRequestResult_ : IO_ERR_TIMEOUT;
    pulseRequestCaller_.clear();
    return result;
}

IoStatus IOModule::resetCounter_(IoId id)
{
    if (id < IO_ID_DI_BASE || id >= IO_ID_DI_BASE + MAX_DIGITAL_INPUTS)
        return IO_ERR_INVALID_ARG;
    return requestPulseSave_(id);
}

IoStatus IOModule::saveCounters_()
{
    return requestPulseSave_(IO_ID_INVALID);
}

bool IOModule::servicePulseRequest_(uint32_t nowMs)
{
    const auto state = pulseRequestState_.load();
    if (state == PulseRequestState::Idle || state == PulseRequestState::Complete) return false;
    const auto finish = [this](IoStatus result) {
        pulseRequestResult_ = result;
        pulseRequestState_.store(PulseRequestState::Complete);
    };
    if (state == PulseRequestState::Requested) {
        // The acquisition cycle may have started before the caller published
        // this request. Use the current clock to avoid unsigned underflow.
        if (uint32_t(millis() - pulseRequestStartedMs_) >= PulseRequestTimeoutMs) {
            finish(IO_ERR_TIMEOUT);
            return true;
        }
        if (pulseRequestResetId_ == IO_ID_INVALID) {
            bool active = false;
            for (uint8_t i = 0; digitalSlots_ && i < MAX_DIGITAL_SLOTS; ++i) {
                const auto& slot = digitalSlots_[i];
                active |= slot.used && slot.kind == DIGITAL_SLOT_INPUT &&
                          slot.inDef.mode == IO_DIGITAL_INPUT_COUNTER && slot.pulse.initialized;
            }
            // Disabled/unconfigured acquisition has no RAM counters to save.
            // Preserve any existing blob, including when recovering through OTA.
            if (!active) { finish(IO_OK); return true; }
        }
        // Drain an older periodic checkpoint before taking the requested snapshot.
        if (pulseReceipt_.status.load() == PersistenceReceipt::Pending) return true;
        if (!runtimeReady_ || !pulseStorageReady_ || !cfgSvc_ || !cfgSvc_->writeRuntimeBlobTracked) {
            finish(IO_ERR_NOT_READY);
            return true;
        }
        checkpointPulses_(nowMs);
        if (pulseReceipt_.status.load() == PersistenceReceipt::Pending) return true;
        if (pulseRequestResetId_ != IO_ID_INVALID) {
            uint8_t index = 0;
            if (!findDigitalSlotByIoId_(pulseRequestResetId_, index)) {
                finish(IO_ERR_UNKNOWN_ID);
                return true;
            }
            auto& slot = digitalSlots_[index];
            if (!cfgData_.enabled || slot.inDef.mode != IO_DIGITAL_INPUT_COUNTER ||
                !slot.provider.isBound() || !slot.endpoint) {
                finish(IO_ERR_NOT_READY);
                return true;
            }
            uint64_t raw = 0;
            auto* driver = static_cast<IDigitalCounterDriver*>(slot.provider.ctx);
            if (!driver || !driver->readCount(raw)) {
                finish(IO_ERR_HW);
                return true;
            }
            // The generation records the reset in the same atomic blob as the count.
            // The legacy configuration token does not need to change.
            slot.pulse.reset(raw, slot.pulse.generation + 1, nowMs);
            (void)processDigitalInputDefinition_(index, nowMs);
        } else {
            for (uint8_t i = 0; i < MAX_DIGITAL_SLOTS; ++i) {
                auto& slot = digitalSlots_[i];
                if (!slot.used || slot.kind != DIGITAL_SLOT_INPUT ||
                    slot.inDef.mode != IO_DIGITAL_INPUT_COUNTER || !slot.pulse.initialized) continue;
                if (!processDigitalInputDefinition_(i, nowMs)) {
                    finish(IO_ERR_HW);
                    return true;
                }
            }
        }
        pulseRequestSubmitted_ = false;
        pulseRequestState_.store(PulseRequestState::Saving);
    }
    if (!pulseRequestSubmitted_) {
        checkpointPulses_(nowMs, true);
        pulseRequestSubmitted_ = true;
        if (pulseRetry_) {
            finish(IO_ERR_PERSISTENCE);
            return true;
        }
    }
    const auto receipt = pulseReceipt_.status.load();
    if (receipt == PersistenceReceipt::Pending) return true;
    finish(receipt == PersistenceReceipt::Failed ? IO_ERR_PERSISTENCE : IO_OK);
    return true;
}

bool IOModule::cmdResetCounter_(void* ctx, const CommandRequest& req, char* reply, size_t len)
{
    StaticJsonDocument<128> doc;
    if (!ctx || !req.json || deserializeJson(doc, req.json) || !doc["id"].is<uint16_t>()) {
        snprintf(reply, len, "{\"ok\":false,\"err\":{\"code\":\"InvalidArg\"}}");
        return false;
    }
    const auto status = static_cast<IOModule*>(ctx)->resetCounter_(doc["id"].as<uint16_t>());
    if (status == IO_OK) {
        snprintf(reply, len, "{\"ok\":true}");
        return true;
    }
    snprintf(reply, len, "{\"ok\":false,\"err\":{\"code\":\"CounterResetFailed\",\"status\":%u}}", unsigned(status));
    return false;
}

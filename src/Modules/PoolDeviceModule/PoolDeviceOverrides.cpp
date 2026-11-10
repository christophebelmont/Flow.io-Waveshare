#include "PoolDeviceModule.h"
#include "Core/ErrorCodes.h"
#include "Core/SpiRamJsonDocument.h"
#include "Core/NvsKeys.h"
#include <esp_timer.h>

namespace {
constexpr const char* DurationKey = NvsKeys::PoolDevice::OverrideDuration;
constexpr uint64_t OverrideCountdownPublishMs = 10000;
uint64_t monotonicMs() { return uint64_t(esp_timer_get_time()) / 1000; }
}

bool PoolDeviceModule::overrideSupported_(uint8_t slot) const
{
    if (slot >= POOL_DEVICE_MAX || !slots_[slot].used) return false;
    const auto& config = slots_[slot].driverConfig;
    if (config.capabilities.kind != PoolControlKind::Relay || config.outputs[0] == IO_ID_INVALID) return false;
    // One owner per output, including outputs used by a multi-speed driver.
    for (uint8_t other = 0; other < POOL_DEVICE_MAX; ++other) {
        if (other == slot || !slots_[other].used) continue;
        const auto& binding = slots_[other].driverConfig;
        for (uint8_t output = 0; output < POOL_MAX_SPEED_STEPS; ++output)
            if (binding.outputs[output] == config.outputs[0]) return false;
    }
    return true;
}

bool PoolDeviceModule::overrideDependenciesSatisfied_(uint8_t slot) const
{
    if (!dependenciesSatisfied_(slot)) return false;
    const auto required = slots_[slot].overridePolicy.requiredOnMask;
    for (uint8_t i = 0; i < POOL_DEVICE_MAX; ++i) {
        if (!(required & (uint16_t(1) << i))) continue;
        const auto& parent = slots_[i];
        if (!parent.used || !parent.def.enabled || !parent.driverReady || !parent.actualOn ||
            parent.feedback.error || parent.feedback.quality == PoolFeedbackQuality::Stale || maxUptimeReached_(parent)) return false;
    }
    return true;
}

uint32_t PoolDeviceModule::overrideBinding_(uint8_t slot) const
{
    const auto endpoint = slots_[slot].driverConfig.outputs[0];
    IoEndpointMeta output{};
    if (ioSvc_ && ioSvc_->meta) ioSvc_->meta(ioSvc_->ctx, endpoint, &output);
    return uint32_t(endpoint) | (uint32_t(output.bindingPort) << 16);
}

uint64_t PoolDeviceModule::overrideUtc_() const
{
    TimeState state{};
    if (!timeSvc_ || !timeSvc_->currentState || !timeSvc_->currentState(timeSvc_->ctx, &state) ||
        !state.valid || (state.quality != TimeQuality::RtcTrusted && state.quality != TimeQuality::NtpSynced)) return 0;
    return state.currentTimeUtc;
}

bool PoolDeviceModule::saveOverride_(void* ctx, const uint8_t* bytes, size_t len)
{
    auto& slot = *static_cast<PoolDeviceSlot*>(ctx);
    if (len != TimedActuatorOverride::RecordSize || !slot.owner->cfgStore_) return false;
    // Versioned timer record followed by the manual baseline. No countdown writes.
    uint8_t record[TimedActuatorOverride::RecordSize + 1];
    memcpy(record, bytes, len);
    record[len] = slot.overrideBaseline ? 1 : 0;
    return slot.owner->cfgStore_->writeRuntimeBlob(slot.overrideKey, record, sizeof(record));
}

bool PoolDeviceModule::saveOverrideDuration_(void* ctx, const uint8_t* bytes, size_t len)
{
    auto& self = *static_cast<PoolDeviceModule*>(ctx);
    return self.cfgStore_ && self.cfgStore_->writeRuntimeBlob(DurationKey, bytes, len);
}

void PoolDeviceModule::restoreOverrides_()
{
    if (!lockState_()) return;
    for (uint8_t i = 0; i < POOL_DEVICE_MAX; ++i) {
        auto& s = slots_[i];
        if (!s.used) continue;
        s.owner = this;
        snprintf(s.overrideKey, sizeof(s.overrideKey), "pdm_ovr_%u", unsigned(i));
        s.overrideTimer.configure(&s, &PoolDeviceModule::saveOverride_);
        uint8_t record[TimedActuatorOverride::RecordSize + 1]{};
        size_t len = 0;
        if (cfgStore_ && cfgStore_->readRuntimeBlob(s.overrideKey, record, sizeof(record), &len)) {
            if (len == sizeof(record) && record[TimedActuatorOverride::RecordSize] <= 1 &&
                s.overrideTimer.restore(record, TimedActuatorOverride::RecordSize)) {
                s.overrideBaseline = record[TimedActuatorOverride::RecordSize] != 0;
                s.overrideRestored = s.overrideTimer.active();
            }
            // A present tombstone always wins over legacy records, including after expiry.
            continue;
        }
        // Migrate the original two leases using their stored slot, not translated roles
        // or current PoolLogic assignment. The destination is durable before adoption.
        for (const auto key : {"ovr_filtr", "ovr_robot"}) {
            if (!cfgStore_ || !cfgStore_->readRuntimeBlob(key, record, sizeof(record), &len) ||
                len != TimedActuatorOverride::RecordSize || record[3] != i) continue;
            TimedActuatorOverride legacy;
            if (!legacy.restore(record, len)) continue;
            if (saveOverride_(&s, record, len)) {
                s.overrideTimer.restore(record, len);
                s.overrideRestored = s.overrideTimer.active();
            } else {
                // A failed migration must not enable a lease, or allow a later
                // reboot to resurrect it after an intervening manual command.
                s.overrideTimer.restore(record, len);
                s.overrideTimer.cancel(ActuatorOverrideReason::Configuration);
            }
            break;
        }
    }
    uint8_t duration[ActuatorOverrideDuration::RecordSize]{};
    size_t len = 0;
    if (cfgStore_ && cfgStore_->readRuntimeBlob(DurationKey, duration, sizeof(duration), &len))
        overrideDuration_.restore(duration, len);
    unlockState_();
}

PoolDeviceSvcStatus PoolDeviceModule::svcSetOverridePoliciesImpl_(const ActuatorOverridePolicy* policies, uint8_t count)
{
    if (!policies || count != POOL_DEVICE_MAX) return POOLDEV_SVC_ERR_INVALID_ARG;
    if (!lockState_()) return POOLDEV_SVC_ERR_NOT_READY;
    for (uint8_t i = 0; i < count; ++i) slots_[i].overridePolicy = policies[i];
    unlockState_();
    return POOLDEV_SVC_OK;
}

PoolDeviceSvcStatus PoolDeviceModule::svcReleaseOverrideImpl_(uint8_t slot, ActuatorOverrideReason reason)
{
    if (!lockState_()) return POOLDEV_SVC_ERR_NOT_READY;
    if (slot >= POOL_DEVICE_MAX || !slots_[slot].used) { unlockState_(); return POOLDEV_SVC_ERR_UNKNOWN_SLOT; }
    const bool saved = slots_[slot].overrideTimer.cancel(reason);
    if (runtimeReady_) tickDevices_(millis(), false);
    unlockState_();
    return saved ? POOLDEV_SVC_OK : POOLDEV_SVC_ERR_IO;
}

uint16_t PoolDeviceModule::svcOverrideDurationImpl_() const
{
    if (!lockState_()) return ActuatorOverrideDuration::DefaultMinutes;
    const auto minutes = overrideDuration_.minutes();
    unlockState_();
    return minutes;
}

bool PoolDeviceModule::resolveOverride_(uint8_t slot, bool safe, uint64_t now, bool& refreshCountdown)
{
    auto& s = slots_[slot];
    auto& timer = s.overrideTimer;
    const auto previous = s.control;
    const bool supported = overrideSupported_(slot);
    if (timer.active() && (!supported || timer.slot() != slot || timer.binding() != overrideBinding_(slot)))
        timer.cancel(ActuatorOverrideReason::Configuration);
    timer.tick(overrideUtc_(), now);
    const auto state = timer.state(now);
    const bool dependenciesReady = overrideDependenciesSatisfied_(slot);
    // A restored request waits for its dependencies to finish their own boot
    // recovery. Once applied, any dependency loss cancels it instead of rearming.
    const bool restoringDependencies = s.overrideRestored && !dependenciesReady &&
        !maxUptimeReached_(s) && !s.feedback.error && s.feedback.quality != PoolFeedbackQuality::Stale;
    const bool safetyCancelled = writesEnabled_ && s.overridePolicy.ready && timer.active() &&
        (!s.def.enabled || !s.driverReady || (state.value ? (!s.overridePolicy.allowOn || ((!safe || !dependenciesReady) && !restoringDependencies)) : !s.overridePolicy.allowOff));
    if (safetyCancelled) {
        timer.cancel(ActuatorOverrideReason::Safety);
        if (state.value) s.desiredOn = s.desired.running = false;
    }
    if (now >= s.overrideRetryMs) { timer.flush(); s.overrideRetryMs = now + 5000; }
    s.control = timer.state(now);
    s.control.supported = supported;
    s.control.automatic = s.overridePolicy.automatic;
    s.control.available = supported && runtimeReady_ && writesEnabled_ && s.overridePolicy.ready &&
        s.def.enabled && s.driverReady && overrideUtc_() && !timer.pendingSave();
    const bool controlChanged = !sameActuatorControl(previous, s.control);
    refreshCountdown = !controlChanged && s.control.mode == ActuatorControlMode::Forced &&
        now - s.overridePublishMs >= OverrideCountdownPublishMs;
    if (controlChanged || refreshCountdown) {
        s.overridePublishMs = now;
        if (dataStore_) dataStore_->notifyChanged(DataKeys::PoolDeviceOverrideBase + slot);
    }
    if (ioSvc_ && ioSvc_->setOutputControlState && supported)
        ioSvc_->setOutputControlState(ioSvc_->ctx, s.driverConfig.outputs[0], slot + 1, &s.control, refreshCountdown);
    if (s.control.mode == ActuatorControlMode::WaitingTime || s.control.mode == ActuatorControlMode::PersistenceError)
        return false;
    if (s.control.mode == ActuatorControlMode::Forced) {
        const bool apply = s.overridePolicy.ready && (!s.control.value || dependenciesReady);
        if (apply && writesEnabled_) s.overrideRestored = false;
        return apply && s.control.value;
    }
    return safetyCancelled && state.value ? false : s.desiredOn;
}

bool PoolDeviceModule::cmdOverride_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolDeviceModule*>(ctx)->svcOverrideCommandImpl_(req, out, len, ActuatorOverrideCommand::FromArgs); }
bool PoolDeviceModule::cmdOverrideOn_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolDeviceModule*>(ctx)->svcOverrideCommandImpl_(req, out, len, ActuatorOverrideCommand::On); }
bool PoolDeviceModule::cmdOverrideOff_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolDeviceModule*>(ctx)->svcOverrideCommandImpl_(req, out, len, ActuatorOverrideCommand::Off); }
bool PoolDeviceModule::cmdOverrideRelease_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolDeviceModule*>(ctx)->svcOverrideCommandImpl_(req, out, len, ActuatorOverrideCommand::Release); }
bool PoolDeviceModule::cmdOverrideDuration_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolDeviceModule*>(ctx)->svcOverrideCommandImpl_(req, out, len, ActuatorOverrideCommand::Duration); }
bool PoolDeviceModule::cmdOverrideSelect_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolDeviceModule*>(ctx)->svcOverrideCommandImpl_(req, out, len, ActuatorOverrideCommand::Select); }

bool PoolDeviceModule::svcOverrideCommandImpl_(const CommandRequest& req, char* reply, size_t replyLen, ActuatorOverrideCommand action)
{
    if (!lockState_()) { writeErrorJson(reply, replyLen, ErrorCode::NotReady, "pooldevice.override"); return false; }
    auto fail = [&](ErrorCode code) { unlockState_(); writeErrorJson(reply, replyLen, code, "pooldevice.override"); return false; };
    SpiRamJsonDocument doc(768);
    const char* json = req.args ? req.args : req.json;
    if (json && deserializeJson(doc, json)) return fail(ErrorCode::BadCmdJson);
    JsonObjectConst args = doc["args"].is<JsonObjectConst>() ? doc["args"].as<JsonObjectConst>() : doc.as<JsonObjectConst>();
    DataKey panelChange = 0;
    if (action == ActuatorOverrideCommand::Duration) {
        if (!args["minutes"].is<uint16_t>() || !args["minutes"].as<uint16_t>() ||
            args["minutes"].as<uint16_t>() > ActuatorOverrideDuration::MaxMinutes) return fail(ErrorCode::BadCmdJson);
        const uint16_t minutes = args["minutes"].as<uint16_t>();
        if (minutes != overrideDuration_.minutes()) panelChange = DataKeys::PoolDeviceOverrideDuration;
        if (!overrideDuration_.set(minutes, this, saveOverrideDuration_)) return fail(ErrorCode::Failed);
    } else {
        uint8_t slot = overrideSelection_;
        const bool explicitSlot = args.containsKey("slot");
        if (explicitSlot) {
            if (!args["slot"].is<uint8_t>()) return fail(ErrorCode::BadSlot);
            slot = args["slot"].as<uint8_t>();
        }
        if (action == ActuatorOverrideCommand::Select) {
            if (explicitSlot || !args["option"].is<const char*>()) return fail(ErrorCode::BadCmdJson);
            slot = 0xFF;
            // Exact membership in the advertised option catalogue; labels are never parsed.
            for (uint8_t i = 0; i < POOL_DEVICE_MAX; ++i)
                if (overrideSupported_(i) && strcmp(args["option"], slots_[i].overrideOption) == 0) { slot = i; break; }
        }
        if (slot >= POOL_DEVICE_MAX || !slots_[slot].used) return fail(ErrorCode::BadSlot);
        auto& s = slots_[slot];
        if (action == ActuatorOverrideCommand::Select) {
            if (overrideSelection_ != slot) panelChange = DataKeys::PoolDeviceOverrides;
            overrideSelection_ = slot;
        } else if (action == ActuatorOverrideCommand::Release) {
            if (!s.overrideTimer.cancel(ActuatorOverrideReason::Released)) {
                tickDevices_(millis(), false); return fail(ErrorCode::Failed);
            }
        } else {
            if (!overrideSupported_(slot)) return fail(ErrorCode::NotReady);
            if (!s.def.enabled) return fail(ErrorCode::Disabled);
            if (!runtimeReady_ || !writesEnabled_ || !s.driverReady || !s.overridePolicy.ready || s.overrideTimer.pendingSave() || !overrideUtc_()) return fail(ErrorCode::NotReady);
            bool value = action == ActuatorOverrideCommand::On;
            if (action == ActuatorOverrideCommand::FromArgs) {
                if (!explicitSlot || !args["value"].is<bool>()) return fail(ErrorCode::BadCmdJson);
                value = args["value"].as<bool>();
            }
            uint32_t duration = overrideDuration_.seconds();
            if (args.containsKey("duration_s")) {
                if (!args["duration_s"].is<uint32_t>()) return fail(ErrorCode::BadCmdJson);
                duration = args["duration_s"].as<uint32_t>();
            } else if (explicitSlot) return fail(ErrorCode::BadCmdJson);
            if (!duration || duration > TimedActuatorOverride::MaxDurationSec) return fail(ErrorCode::BadCmdJson);
            if ((value && (!s.overridePolicy.allowOn || !overrideDependenciesSatisfied_(slot) || maxUptimeReached_(s) ||
                s.feedback.error || s.feedback.quality == PoolFeedbackQuality::Stale)) ||
                (!value && !s.overridePolicy.allowOff)) return fail(ErrorCode::InterlockBlocked);
            s.interlockState = PoolInterlockState::Ready;
            const bool previousBaseline = s.overrideBaseline;
            s.overrideBaseline = s.desiredOn;
            if (!s.overrideTimer.start(slot, overrideBinding_(slot), value, duration, overrideUtc_(), monotonicMs())) {
                s.overrideBaseline = previousBaseline;
                return fail(ErrorCode::Failed);
            }
            s.overrideRestored = false;
            char title[ACTIVITY_TITLE_MAX]{};
            char detail[ACTIVITY_DETAIL_MAX]{};
            snprintf(title, sizeof(title), "%s : forçage %s demandé", s.def.label, value ? "ON" : "OFF");
            snprintf(detail, sizeof(detail), "Forçage %s de %s (%s) pendant %lu s, enregistré. Retour guidé à l'échéance.",
                value ? "ON" : "OFF", s.def.label, s.id, (unsigned long)duration);
            emitActivity_(value ? ActivityCode::PoolLogicOverrideOnRequested : ActivityCode::PoolLogicOverrideOffRequested,
                ActivitySource::Manual, ActivitySeverity::Info, ActivityRole::None,
                value ? ActivityState::RequestedOn : ActivityState::RequestedOff, ActivityReason::Manual,
                slot, title, detail, "back_hand", req.actor);
        }
    }
    if (runtimeReady_) tickDevices_(millis(), false);
    if (dataStore_ && panelChange) dataStore_->notifyChanged(panelChange);
    snprintf(reply, replyLen, "{\"ok\":true,\"duration_minutes\":%u,\"persisted\":%s}",
        unsigned(overrideDuration_.minutes()), action == ActuatorOverrideCommand::Select ? "false" : "true");
    unlockState_();
    return true;
}

bool PoolDeviceModule::buildOverrideSnapshot_(bool all, char* out, size_t len, uint32_t& maxTsOut) const
{
    if (!lockState_()) return false;
    SpiRamJsonDocument doc(all ? 12288 : 1536);
    const auto now = monotonicMs();
    auto writeDevice = [&](JsonObject object, uint8_t slot) {
        const auto& s = slots_[slot];
        auto state = s.overrideTimer.state(now);
        state.supported = s.control.supported;
        state.available = s.control.available;
        state.automatic = s.control.automatic;
        writeActuatorControlJson(object, state);
        object["slot"] = slot;
        object["name"] = s.def.label;
        object["actual_on"] = s.actualOn;
        object["guided_on"] = s.desiredOn;
    };
    if (all) {
        auto actuators = doc.createNestedObject("actuators");
        unsigned count = 0;
        for (uint8_t i = 0; i < POOL_DEVICE_MAX; ++i) {
            if (!overrideSupported_(i)) continue;
            writeDevice(actuators.createNestedObject(slots_[i].id), i);
            if (slots_[i].overrideTimer.active()) ++count;
        }
        doc["active_count"] = count;
    } else {
        if (overrideSelection_ < POOL_DEVICE_MAX) {
            writeDevice(doc.to<JsonObject>(), overrideSelection_);
            doc["id"] = slots_[overrideSelection_].id;
            doc["selection"] = slots_[overrideSelection_].overrideOption;
        } else {
            writeActuatorControlJson(doc, ActuatorControlState{});
            doc["selection"] = "";
        }
        doc["duration_minutes"] = overrideDuration_.minutes();
    }
    const bool ok = !doc.overflowed() && measureJson(doc) < len;
    if (ok) serializeJson(doc, out, len);
    maxTsOut = millis() ? millis() : 1;
    unlockState_();
    return ok;
}

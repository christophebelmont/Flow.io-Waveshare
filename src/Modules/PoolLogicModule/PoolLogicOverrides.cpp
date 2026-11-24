#include "PoolLogicModule.h"
#include "Core/ErrorCodes.h"
#include "Core/CommandRegistry.h"
#include "Core/SpiRamJsonDocument.h"
#include "Modules/PoolDeviceModule/PoolDeviceModuleDataModel.h"

// Keep the existing role-targeted MQTT API while PoolDevice owns every lease.
bool PoolLogicModule::guidedDeviceOn_(uint8_t slot, bool observed) const
{
    PoolDeviceSvcMeta meta{};
    if (poolSvc_ && poolSvc_->meta && poolSvc_->meta(poolSvc_->ctx, slot, &meta) == POOLDEV_SVC_OK) return meta.guidedOn;
    return observed;
}

void PoolLogicModule::resetOverridePolicies_()
{
    if (!poolSvc_ || !poolSvc_->setOverridePolicies) return;
    ActuatorOverridePolicy policies[POOL_DEVICE_MAX]{};
    {
        for (auto slot : {filtrationDeviceSlot_, robotDeviceSlot_, phPumpDeviceSlot_, orpPumpDeviceSlot_,
                          swgDeviceSlot_, heaterDeviceSlot_, fillingDeviceSlot_}) {
            if (slot < POOL_DEVICE_MAX) policies[slot].ready = false;
        }
    }
    poolSvc_->setOverridePolicies(poolSvc_->ctx, policies, POOL_DEVICE_MAX);
}

bool PoolLogicModule::buildOverrideSnapshot_(uint8_t index, char* out, size_t len, uint32_t& maxTsOut) const
{
    OverrideLock lock(overrideMutex_);
    const uint8_t slot = index == 0 ? filtrationDeviceSlot_ : robotDeviceSlot_;
    PoolDeviceSvcMeta meta{};
    if (!poolSvc_ || !poolSvc_->meta || poolSvc_->meta(poolSvc_->ctx, slot, &meta) != POOLDEV_SVC_OK) return false;
    SpiRamJsonDocument doc(768);
    writeActuatorControlJson(doc, meta.control);
    doc["slot"] = slot;
    doc["duration_minutes"] = poolSvc_->overrideDuration ? poolSvc_->overrideDuration(poolSvc_->ctx) : 30;
    if (doc.overflowed() || measureJson(doc) >= len) return false;
    serializeJson(doc, out, len);
    maxTsOut = millis() ? millis() : 1;
    return true;
}

bool PoolLogicModule::cmdOverride_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolLogicModule*>(ctx)->handleOverride_(req, out, len, OverrideCommand::FromArgs); }
bool PoolLogicModule::cmdOverrideOn_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolLogicModule*>(ctx)->handleOverride_(req, out, len, OverrideCommand::On); }
bool PoolLogicModule::cmdOverrideOff_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolLogicModule*>(ctx)->handleOverride_(req, out, len, OverrideCommand::Off); }
bool PoolLogicModule::cmdRelease_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolLogicModule*>(ctx)->handleOverride_(req, out, len, OverrideCommand::Release); }
bool PoolLogicModule::cmdOverrideDuration_(void* ctx, const CommandRequest& req, char* out, size_t len)
{ return static_cast<PoolLogicModule*>(ctx)->handleOverride_(req, out, len, OverrideCommand::Duration); }

bool PoolLogicModule::handleOverride_(const CommandRequest& req, char* reply, size_t len, OverrideCommand command)
{
    OverrideLock lock(overrideMutex_);
    auto fail = [&](ErrorCode code) { writeErrorJson(reply, len, code, "poollogic.device.override"); return false; };
    if (!poolSvc_ || !poolSvc_->overrideCommand) return fail(ErrorCode::NotReady);
    SpiRamJsonDocument doc(768);
    const char* json = req.args ? req.args : req.json;
    if (!json || deserializeJson(doc, json)) return fail(ErrorCode::MissingArgs);
    JsonObject args = doc["args"].is<JsonObject>() ? doc["args"].as<JsonObject>() : doc.as<JsonObject>();
    if (args.containsKey("role")) {
        if (args.containsKey("slot") || !args["role"].is<const char*>()) return fail(ErrorCode::BadSlot);
        // Stable protocol role identifiers, independent of display names or locale.
        const char* role = args["role"];
        uint8_t slot;
        if (strcmp(role, "filtration") == 0) slot = filtrationDeviceSlot_;
        else if (strcmp(role, "robot") == 0) slot = robotDeviceSlot_;
        else return fail(ErrorCode::BadSlot);
        args.remove("role");
        args["slot"] = slot;
        if ((command == OverrideCommand::On || command == OverrideCommand::Off) && !args.containsKey("duration_s"))
            args["duration_s"] = uint32_t(poolSvc_->overrideDuration(poolSvc_->ctx)) * 60;
    } else if (!args["slot"].is<uint8_t>()) return fail(ErrorCode::BadSlot);
    char normalized[512]{};
    if (measureJson(args) >= sizeof(normalized)) return fail(ErrorCode::BadCmdJson);
    serializeJson(args, normalized, sizeof(normalized));
    const CommandRequest forwarded{req.cmd, nullptr, normalized, req.actor};
    return poolSvc_->overrideCommand(poolSvc_->ctx, forwarded, reply, len, command);
}

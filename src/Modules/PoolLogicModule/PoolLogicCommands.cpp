/**
 * @file PoolLogicCommands.cpp
 * @brief Command handlers for PoolLogicModule.
 */

#include "PoolLogicModule.h"
#include "ManualDeviceCommand.h"
#include "Modules/PoolDeviceModule/PoolDeviceModuleDataModel.h"
#include "Core/CommandRegistry.h"
#include "Core/ErrorCodes.h"
#include "Core/SystemLimits.h"

#include "Core/SpiRamJsonDocument.h"
#include <cstdlib>
#include <cstring>
#include <stdio.h>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::PoolLogicModule)
#include "Core/ModuleLog.h"

namespace {
// Commands may send either a compact args JSON or a full root payload with an
// "args" object. This helper accepts both shapes to preserve compatibility.
static bool parseCmdArgsObject_(const CommandRequest& req, JsonDocument& doc, JsonObjectConst& outObj)
{
    if (doc.capacity() == 0U) return false;

    doc.clear();
    const char* json = req.args ? req.args : req.json;
    if (!json || json[0] == '\0') return false;

    const DeserializationError err = deserializeJson(doc, json);
    if (!err && doc.is<JsonObject>()) {
        outObj = doc["args"].is<JsonObjectConst>()
            ? doc["args"].as<JsonObjectConst>() : doc.as<JsonObjectConst>();
        return true;
    }

    if (req.json && req.json[0] != '\0' && req.args != req.json) {
        doc.clear();
        const DeserializationError rootErr = deserializeJson(doc, req.json);
        if (rootErr || !doc.is<JsonObjectConst>()) return false;
        JsonVariantConst argsVar = doc["args"];
        if (argsVar.is<JsonObjectConst>()) {
            outObj = argsVar.as<JsonObjectConst>();
            return true;
        }
    }

    return false;
}

static bool parseBoolValue_(JsonVariantConst value, bool& out)
{
    if (value.is<bool>()) {
        out = value.as<bool>();
        return true;
    }
    if (value.is<int32_t>() || value.is<uint32_t>() || value.is<float>()) {
        out = (value.as<float>() != 0.0f);
        return true;
    }
    if (value.is<const char*>()) {
        const char* s = value.as<const char*>();
        if (!s) s = "0";
        if (strcmp(s, "true") == 0) out = true;
        else if (strcmp(s, "false") == 0) out = false;
        else out = (atoi(s) != 0);
        return true;
    }
    return false;
}

static void writeCmdError_(char* reply, size_t replyLen, const char* where, ErrorCode code)
{
    if (!writeErrorJson(reply, replyLen, code, where)) {
        snprintf(reply, replyLen, "{\"ok\":false}");
    }
}
}

// Thin static trampolines keep the command registry wiring stable while the
// actual logic lives in instance methods.
bool PoolLogicModule::cmdFiltrationWriteStatic_(void* userCtx,
                                                const CommandRequest& req,
                                                char* reply,
                                                size_t replyLen)
{
    PoolLogicModule* self = static_cast<PoolLogicModule*>(userCtx);
    if (!self) return false;
    return self->cmdFiltrationWrite_(req, reply, replyLen);
}

bool PoolLogicModule::cmdOperatingModeSetStatic_(void* userCtx,
                                            const CommandRequest& req,
                                            char* reply,
                                            size_t replyLen)
{
    PoolLogicModule* self = static_cast<PoolLogicModule*>(userCtx);
    if (!self) return false;
    return self->cmdOperatingModeSet_(req, reply, replyLen);
}

bool PoolLogicModule::cmdFiltrationRecalcStatic_(void* userCtx,
                                                 const CommandRequest& req,
                                                 char* reply,
                                                 size_t replyLen)
{
    PoolLogicModule* self = static_cast<PoolLogicModule*>(userCtx);
    if (!self) return false;
    return self->cmdFiltrationRecalc_(req, reply, replyLen);
}

bool PoolLogicModule::cmdMqttControlStatic_(void* userCtx,
                                            const CommandRequest& req,
                                            char* reply,
                                            size_t replyLen)
{
    PoolLogicModule* self = static_cast<PoolLogicModule*>(userCtx);
    if (!self) return false;
    return self->cmdMqttControl_(req, reply, replyLen);
}

bool PoolLogicModule::cmdDeviceWriteStatic_(void* userCtx,
                                          const CommandRequest& req,
                                          char* reply,
                                          size_t replyLen)
{
    PoolLogicModule* self = static_cast<PoolLogicModule*>(userCtx);
    return self && self->cmdDeviceWrite_(req, reply, replyLen);
}

bool PoolLogicModule::cmdDeviceWrite_(const CommandRequest& req, char* reply, size_t replyLen)
{
    if (!commandSvc_ || !commandSvc_->execute) {
        writeCmdError_(reply, replyLen, "poollogic.device.write", ErrorCode::NotReady);
        return false;
    }
    SpiRamJsonDocument argsDoc(Limits::JsonCmdPoolDeviceBuf);
    JsonObjectConst args;
    if (!parseCmdArgsObject_(req, argsDoc, args)) {
        writeCmdError_(reply, replyLen, "poollogic.device.write", ErrorCode::MissingArgs);
        return false;
    }
    if (!args.containsKey("slot")) {
        writeCmdError_(reply, replyLen, "poollogic.device.write", ErrorCode::MissingSlot);
        return false;
    }
    if (!args["slot"].is<uint8_t>() || args["slot"].as<uint8_t>() >= POOL_DEVICE_MAX) {
        writeCmdError_(reply, replyLen, "poollogic.device.write", ErrorCode::BadSlot);
        return false;
    }
    const PoolManualDeviceSlots roles{filtrationDeviceSlot_, phPumpDeviceSlot_, orpPumpDeviceSlot_,
                                     robotDeviceSlot_, heaterDeviceSlot_, swgDeviceSlot_};
    const char* command = poolManualDeviceWriteCommand(args["slot"].as<uint8_t>(), roles);
    return commandSvc_->execute(commandSvc_->ctx, command, req.json, req.args, req.actor, reply, replyLen);
}

bool PoolLogicModule::cmdFiltrationWrite_(const CommandRequest& req, char* reply, size_t replyLen)
{
    return cmdMqttControl_(req, reply, replyLen);
}

bool PoolLogicModule::cmdFiltrationRecalc_(const CommandRequest&, char* reply, size_t replyLen)
{
    portENTER_CRITICAL(&pendingMux_);
    pendingDailyRecalc_ = true;
    portEXIT_CRITICAL(&pendingMux_);
    snprintf(reply, replyLen, "{\"ok\":true,\"queued\":true}");
    return true;
}

bool PoolLogicModule::cmdOperatingModeSet_(const CommandRequest& req, char* reply, size_t replyLen)
{
    OverrideLock lock(overrideMutex_);
    constexpr const char* command = "poollogic.operating_mode.set";
    if (!cfgStore_) {
        writeCmdError_(reply, replyLen, command, ErrorCode::NotReady);
        return false;
    }
    SpiRamJsonDocument doc(Limits::JsonCmdPoolDeviceBuf);
    JsonObjectConst args;
    if (!parseCmdArgsObject_(req, doc, args)) {
        writeCmdError_(reply, replyLen, command, ErrorCode::MissingArgs);
        return false;
    }
    if (!args["value"].is<uint8_t>() || !poolModeValid(args["value"].as<uint8_t>())) {
        writeCmdError_(reply, replyLen, command, ErrorCode::InvalidMode);
        return false;
    }
    const uint8_t requested = args["value"];
    if (!cfgStore_->set(operatingModeVar_, requested)) {
        writeCmdError_(reply, replyLen, command, ErrorCode::Failed);
        return false;
    }
    snprintf(reply, replyLen, "{\"ok\":true,\"value\":%u}", unsigned(requested));
    return true;
}

bool PoolLogicModule::cmdMqttControl_(const CommandRequest& req, char* reply, size_t replyLen)
{
    OverrideLock lock(overrideMutex_);
    if (!req.cmd || !commandSvc_ || !commandSvc_->execute) {
        writeCmdError_(reply, replyLen, "poollogic.cmd", ErrorCode::NotReady);
        return false;
    }
    // Exact protocol command dispatch; labels and translated equipment names
    // never determine the business role or its permissions.
    struct DeviceCommands { const char* write; const char* toggle; uint8_t slot; };
    const DeviceCommands devices[] = {
        {"poollogic.filtration.write", "poollogic.filtration.toggle", filtrationDeviceSlot_},
        {"poollogic.ph_pump.write", "poollogic.ph_pump.toggle", phPumpDeviceSlot_},
        {"poollogic.dis_pump.write", "poollogic.dis_pump.toggle", orpPumpDeviceSlot_},
        {"poollogic.orp_pump.write", "poollogic.orp_pump.toggle", orpPumpDeviceSlot_},
        {"poollogic.robot.write", "poollogic.robot.toggle", robotDeviceSlot_},
        {"poollogic.fill_pump.write", "poollogic.fill_pump.toggle", fillingDeviceSlot_},
        {"poollogic.heater.write", "poollogic.heater.toggle", heaterDeviceSlot_},
        {"poollogic.chlorine_generator.write", "poollogic.chlorine_generator.toggle", swgDeviceSlot_},
        {"poollogic.swg.write", "poollogic.swg.toggle", swgDeviceSlot_},
        {"poollogic.lights.write", "poollogic.lights.toggle", PoolIds::DeviceLights},
        {"poollogic.light.write", "poollogic.light.toggle", PoolIds::DeviceLights},
    };
    for (const auto& device : devices) {
        const bool toggle = strcmp(req.cmd, device.toggle) == 0;
        if (!toggle && strcmp(req.cmd, device.write) != 0) continue;
        SpiRamJsonDocument doc(Limits::JsonCmdPoolDeviceBuf);
        JsonObjectConst parsed;
        bool value = false;
        if (toggle) {
            if (!readDeviceActualOn_(device.slot, value)) {
                writeCmdError_(reply, replyLen, req.cmd, ErrorCode::NotReady);
                return false;
            }
            value = !value;
            doc.to<JsonObject>();
        } else if (!parseCmdArgsObject_(req, doc, parsed) || !parseBoolValue_(parsed["value"], value)) {
            writeCmdError_(reply, replyLen, req.cmd, ErrorCode::MissingValue);
            return false;
        }
        JsonObject normalized = doc["args"].is<JsonObject>()
            ? doc["args"].as<JsonObject>() : doc.as<JsonObject>();
        normalized["slot"] = device.slot;
        normalized["value"] = value;
        char args[Limits::JsonCmdPoolDeviceBuf];
        if (doc.overflowed() || measureJson(normalized) >= sizeof(args)) {
            writeCmdError_(reply, replyLen, req.cmd, ErrorCode::BadCmdJson);
            return false;
        }
        serializeJson(normalized, args, sizeof(args));
        return commandSvc_->execute(commandSvc_->ctx, "pooldevice.write", nullptr, args, req.actor, reply, replyLen);
    }
    if (strcmp(req.cmd, "poollogic.heater_auto_mode.set") == 0 ||
        strcmp(req.cmd, "poollogic.heater_auto_mode.toggle") == 0) {
        bool value = !heaterAutoMode_;
        if (strcmp(req.cmd, "poollogic.heater_auto_mode.set") == 0) {
            SpiRamJsonDocument doc(Limits::JsonCmdPoolDeviceBuf);
            JsonObjectConst args;
            if (!parseCmdArgsObject_(req, doc, args) || !parseBoolValue_(args["value"], value)) {
                writeCmdError_(reply, replyLen, req.cmd, ErrorCode::MissingValue);
                return false;
            }
        }
        if (!cfgStore_ || !cfgStore_->set(heaterAutoModeVar_, value)) {
            writeCmdError_(reply, replyLen, req.cmd, ErrorCode::Failed);
            return false;
        }
        snprintf(reply, replyLen, "{\"ok\":true,\"value\":%s}", value ? "true" : "false");
        return true;
    }
    writeCmdError_(reply, replyLen, req.cmd, ErrorCode::UnknownCmd);
    return false;
}

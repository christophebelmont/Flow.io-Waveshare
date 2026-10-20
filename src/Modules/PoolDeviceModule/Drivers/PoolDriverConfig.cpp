#include "PoolDriverConfig.h"
#include "PoolDeviceDriver.h"
#include "PoolSerialProfiles.h"
#include "Core/SpiRamJsonDocument.h"
#include <cstdio>
#include <cstring>

namespace {
template<class T> bool field(JsonObjectConst object, const char* key, T& value)
{
    if (!object.containsKey(key)) return true;
    if (!object[key].is<T>()) return false;
    value = object[key].as<T>(); return true;
}
template<class T> bool enumField(JsonObjectConst object, const char* key, T& value, T maximum)
{
    uint8_t raw = uint8_t(value);
    if (!field(object, key, raw) || raw > uint8_t(maximum)) return false;
    value = T(raw);
    return true;
}
bool operation(JsonObjectConst root, const char* name, PoolRegisterOperation& op)
{
    if (!root.containsKey(name)) return true;
    if (!root[name].is<JsonObjectConst>()) return false;
    auto obj = root[name].as<JsonObjectConst>();
    if (!field(obj, "address", op.address) || !field(obj, "function", op.function) ||
        !enumField(obj, "layout", op.operation, RegisterOperation::WriteMultiple) ||
        !enumField(obj, "response", op.responseLayout, RegisterResponseLayout::AddressByteCount)) return false;
    return true;
}
}
bool parsePoolDriverConfig(const char* json, PoolDriverConfig& out, char* error, size_t errorSize)
{
    auto fail = [&](const char* reason) { if (error && errorSize) snprintf(error, errorSize, "%s", reason); return false; };
    SpiRamJsonDocument doc(6144);
    if (!json || deserializeJson(doc, json) || !doc.is<JsonObject>()) return fail("driver must be a JSON object");
    const auto obj = doc.as<JsonObjectConst>();
    PoolDriverConfig c{};
    uint8_t mode = 0, unit = 0;
    if (!obj["kind"].is<uint8_t>() || !field(obj, "kind", mode) || mode > 3 ||
        !field(obj, "unit", unit) || unit > uint8_t(PoolSetpointUnit::Rpm)) return fail("invalid driver kind/unit");
    auto& caps = c.capabilities;
    caps.kind = PoolControlKind(mode); caps.unit = PoolSetpointUnit(unit);
    if (!field(obj, "minimum", caps.minimum) || !field(obj, "maximum", caps.maximum) ||
        !field(obj, "startup", caps.startup) || !field(obj, "dead_ms", c.breakBeforeMakeMs) ||
        !field(obj, "off", c.analogOff) || !field(obj, "gain", c.analogGain) || !field(obj, "offset", c.analogOffset) ||
        !field(obj, "dependency_minimum", c.dependencyMinimum) ||
        !field(obj, "dependency_confirmed", c.requireConfirmedDependency)) return fail("invalid control parameters");
    if (caps.kind != PoolControlKind::Rs485) {
        if (!obj["outputs"].is<JsonArrayConst>()) return fail("outputs array required");
        auto outputs = obj["outputs"].as<JsonArrayConst>();
        if (outputs.size() == 0 || outputs.size() > POOL_MAX_SPEED_STEPS ||
            (caps.kind != PoolControlKind::Discrete && outputs.size() != 1)) return fail("invalid output count");
        uint8_t i = 0;
        for (auto output : outputs) {
            if (!output.is<uint16_t>()) return fail("output must be an IoId");
            c.outputs[i++] = output.as<uint16_t>();
        }
        if (caps.kind == PoolControlKind::Discrete) {
            if (!obj["steps"].is<JsonArrayConst>()) return fail("steps array required");
            auto steps = obj["steps"].as<JsonArrayConst>();
            if (steps.size() != outputs.size()) return fail("steps and outputs must have the same size");
            caps.stepCount = i; i = 0;
            for (auto step : steps) {
                if (!step.is<float>()) return fail("steps must be numeric");
                caps.steps[i++] = step.as<float>();
            }
        }
    } else {
        if (!obj["serial"].is<JsonObjectConst>()) return fail("serial object required");
        auto serial = obj["serial"].as<JsonObjectConst>();
        if (serial.containsKey("model")) {
            uint8_t model = 0;
            if (serial.size() != 1 || !field(serial, "model", model) ||
                !applyPoolSerialProfile(PoolSerialModel(model), c)) return fail("invalid device model or profile overrides");
        } else {
            auto& s = c.serial;
            uint8_t protocol = 0;
            int32_t baud = int32_t(s.line.baud), quiet = int32_t(s.line.quietMs), guard = int32_t(s.line.lateResponseGuardMs);
            int32_t timeout = int32_t(s.timeoutMs), poll = int32_t(s.pollMs), stale = int32_t(s.staleMs);
            if (!field(serial, "protocol", protocol) || protocol > 1 || !field(serial, "bus", s.line.busId) ||
                !field(serial, "baud", baud) || !field(serial, "parity", s.line.parity) ||
                !field(serial, "stop_bits", s.line.stopBits) || !field(serial, "quiet_ms", quiet) ||
                !field(serial, "late_guard_ms", guard) || !field(serial, "address", s.address) ||
                !field(serial, "timeout_ms", timeout) || !field(serial, "poll_ms", poll) ||
                !field(serial, "stale_ms", stale) || !field(serial, "retries", s.retries) ||
                !field(serial, "has_feedback", s.hasFeedback) || !field(serial, "run_value", s.runValue) ||
                !enumField(serial, "control", s.control, PoolSerialControl::SetpointOrStop) ||
                !enumField(serial, "running_source", s.runningSource, PoolRunningSource::FeedbackThreshold) ||
                !enumField(serial, "feedback_type", s.feedbackType, PoolRegisterValueType::Signed16) ||
                !enumField(serial, "raw_rounding", s.rawRounding, PoolRawRounding::Down) ||
                !enumField(serial, "telemetry_profile", s.telemetryProfile, PoolTelemetryProfile::HeatPumpPoly) ||
                !field(serial, "telemetry_stale_ms", s.telemetryStaleMs) ||
                !field(serial, "running_threshold", s.runningThreshold) || !field(serial, "raw_step", s.rawStep) ||
                !field(serial, "stop_value", s.stopValue) || !field(serial, "running_mask", s.runningMask) ||
                !field(serial, "raw_per_unit", s.rawPerUnit) || !field(serial, "raw_offset", s.rawOffset) ||
                !field(serial, "feedback_gain", s.feedbackUnitsPerRaw) || !field(serial, "feedback_offset", s.feedbackOffset) ||
                !operation(serial, "run", s.run) || !operation(serial, "setpoint", s.setpoint) ||
                !operation(serial, "status", s.status) || !operation(serial, "feedback", s.feedback)) return fail("invalid serial parameters");
            if (baud < 0 || quiet < 0 || guard < 0 || timeout < 0 || poll < 0 || stale < 0) return fail("negative serial timing");
            s.protocol = RegisterWireProtocol(protocol); s.line.baud = baud; s.line.quietMs = quiet;
            s.line.lateResponseGuardMs = guard; s.timeoutMs = timeout; s.pollMs = poll; s.staleMs = stale;
            if (serial.containsKey("run_modes")) {
                if (!serial["run_modes"].is<JsonArrayConst>()) return fail("run_modes must be an array");
                const auto modes = serial["run_modes"].as<JsonArrayConst>();
                if (modes.size() > POOL_MAX_RUN_MODES) return fail("too many run modes");
                for (auto mode : modes) {
                    if (!mode.is<JsonObjectConst>() || !mode["label"].is<const char*>() ||
                        !mode["value"].is<uint16_t>()) return fail("run mode requires label and value");
                    auto& option = s.modes.options[s.modes.count++];
                    const char* label = mode["label"].as<const char*>();
                    if (!label[0] || strlen(label) >= sizeof(option.label)) return fail("invalid run mode label");
                    strcpy(option.label, label); option.value = mode["value"].as<uint16_t>();
                }
            }
        }
    }
    if (obj.containsKey("flow_curve")) {
        if (!obj["flow_curve"].is<JsonArrayConst>()) return fail("flow_curve must be an array");
        auto curve = obj["flow_curve"].as<JsonArrayConst>();
        if (curve.size() > POOL_MAX_SPEED_STEPS) return fail("too many flow points");
        for (auto point : curve) {
            if (!point.is<JsonArrayConst>() || point.size() != 2 || !point[0].is<float>() || !point[1].is<float>())
                return fail("flow point must contain setpoint and litres/hour");
            c.flowPoints[c.flowPointCount++] = {point[0].as<float>(), point[1].as<float>()};
        }
    }
    if (!validatePoolDriverConfig(c)) return fail("driver limits, steps or protocol are inconsistent");
    out = c;
    if (error && errorSize) error[0] = '\0';
    return true;
}

bool serializePoolDriverConfig(const PoolDriverConfig& c, char* out, size_t size)
{
    if (!out || !size || !validatePoolDriverConfig(c)) return false;
    SpiRamJsonDocument doc(6144);
    doc["kind"] = uint8_t(c.capabilities.kind); doc["unit"] = uint8_t(c.capabilities.unit);
    doc["minimum"] = c.capabilities.minimum; doc["maximum"] = c.capabilities.maximum;
    doc["startup"] = c.capabilities.startup;
    doc["dependency_minimum"] = c.dependencyMinimum; doc["dependency_confirmed"] = c.requireConfirmedDependency;
    if (c.flowPointCount) {
        auto curve = doc.createNestedArray("flow_curve");
        for (uint8_t i = 0; i < c.flowPointCount; ++i) {
            auto point = curve.createNestedArray(); point.add(c.flowPoints[i].setpoint); point.add(c.flowPoints[i].litresPerHour);
        }
    }
    if (c.capabilities.kind != PoolControlKind::Rs485) {
        auto outputs = doc.createNestedArray("outputs");
        const uint8_t count = c.capabilities.kind == PoolControlKind::Discrete ? c.capabilities.stepCount : 1;
        for (uint8_t i = 0; i < count; ++i) outputs.add(c.outputs[i]);
        if (c.capabilities.kind == PoolControlKind::Discrete) {
            auto steps = doc.createNestedArray("steps");
            for (uint8_t i = 0; i < count; ++i) steps.add(c.capabilities.steps[i]);
            doc["dead_ms"] = c.breakBeforeMakeMs;
        } else if (c.capabilities.kind == PoolControlKind::Analog) {
            doc["gain"] = c.analogGain; doc["offset"] = c.analogOffset; doc["off"] = c.analogOff;
        }
    } else {
        const auto& s = c.serial;
        auto serial = doc.createNestedObject("serial");
        if (s.model != PoolSerialModel::Custom) {
            serial["model"] = uint8_t(s.model);
            doc.remove("unit"); doc.remove("minimum"); doc.remove("maximum"); doc.remove("startup");
        } else {
            serial["protocol"] = uint8_t(s.protocol); serial["bus"] = s.line.busId; serial["address"] = s.address;
            serial["baud"] = s.line.baud; serial["parity"] = s.line.parity; serial["stop_bits"] = s.line.stopBits;
            serial["quiet_ms"] = s.line.quietMs; serial["late_guard_ms"] = s.line.lateResponseGuardMs;
            serial["timeout_ms"] = s.timeoutMs; serial["poll_ms"] = s.pollMs; serial["stale_ms"] = s.staleMs;
            serial["retries"] = s.retries; serial["has_feedback"] = s.hasFeedback;
            serial["control"] = uint8_t(s.control); serial["running_source"] = uint8_t(s.runningSource);
            serial["feedback_type"] = uint8_t(s.feedbackType); serial["running_threshold"] = s.runningThreshold;
            serial["raw_step"] = s.rawStep; serial["raw_rounding"] = uint8_t(s.rawRounding);
            serial["telemetry_profile"] = uint8_t(s.telemetryProfile); serial["telemetry_stale_ms"] = s.telemetryStaleMs;
            if (s.modes.count) {
                auto modes = serial.createNestedArray("run_modes");
                for (uint8_t i = 0; i < s.modes.count; ++i) {
                    auto mode = modes.createNestedObject();
                    mode["label"] = s.modes.options[i].label; mode["value"] = s.modes.options[i].value;
                }
            }
            serial["run_value"] = s.runValue; serial["stop_value"] = s.stopValue; serial["running_mask"] = s.runningMask;
            serial["raw_per_unit"] = s.rawPerUnit; serial["raw_offset"] = s.rawOffset;
            serial["feedback_gain"] = s.feedbackUnitsPerRaw; serial["feedback_offset"] = s.feedbackOffset;
            const char* names[] = {"run", "setpoint", "status", "feedback"};
            const PoolRegisterOperation* ops[] = {&s.run, &s.setpoint, &s.status, &s.feedback};
            for (uint8_t i = 0; i < 4; ++i) {
                auto op = serial.createNestedObject(names[i]);
                op["address"] = ops[i]->address; op["function"] = ops[i]->function; op["layout"] = uint8_t(ops[i]->operation);
                op["response"] = uint8_t(ops[i]->responseLayout);
            }
        }
    }
    if (doc.overflowed() || measureJson(doc) >= size) return false;
    serializeJson(doc, out, size); return true;
}

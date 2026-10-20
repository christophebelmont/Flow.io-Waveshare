#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include "Modules/PoolDeviceModule/Drivers/PoolDriverConfig.h"
#include "Modules/PoolDeviceModule/Drivers/PoolSerialHa.h"
#include "Modules/PoolDeviceModule/Drivers/PoolTelemetryJson.h"
int main() {
    PoolDriverConfig c; char error[100];
    assert(parsePoolDriverConfig("{\"kind\":0,\"outputs\":[0]}",c,error,sizeof(error)));
    assert(parsePoolDriverConfig("{\"kind\":1,\"outputs\":[8,9,10],\"steps\":[30,60,100],\"startup\":60}",c,error,sizeof(error)));
    assert(!parsePoolDriverConfig("{\"kind\":1,\"outputs\":[8,9],\"steps\":[60,30]}",c,error,sizeof(error)));
    assert(!parsePoolDriverConfig("{\"kind\":1,\"outputs\":[8,8],\"steps\":[60,100]}",c,error,sizeof(error)));
    assert(!parsePoolDriverConfig("{\"kind\":0,\"outputs\":[-1]}",c,error,sizeof(error)));
    assert(!parsePoolDriverConfig("{\"kind\":3,\"serial\":{\"baud\":-1}}",c,error,sizeof(error)));
    assert(!parsePoolDriverConfig("{\"kind\":3,\"serial\":{\"run\":{\"function\":208}}}",c,error,sizeof(error)));
    assert(parsePoolDriverConfig("{\"kind\":3,\"serial\":{\"protocol\":1,\"baud\":19200,\"run\":{\"function\":208},\"setpoint\":{\"function\":208},\"status\":{\"function\":195},\"feedback\":{\"function\":195},\"has_feedback\":true}}",c,error,sizeof(error)));
    assert(parsePoolDriverConfig("{\"kind\":2,\"outputs\":[256],\"flow_curve\":[[0,0],[50,800],[100,2400]]}",c,error,sizeof(error)));
    assert(c.flowPointCount==3);
    char encoded[POOL_DRIVER_CONFIG_BYTES]; PoolDriverConfig decoded;
    assert(serializePoolDriverConfig(c,encoded,sizeof(encoded)));
    assert(parsePoolDriverConfig(encoded,decoded,error,sizeof(error)) && decoded.flowPointCount==3);
    assert(parsePoolDriverConfig("{\"kind\":3,\"serial\":{\"baud\":19200}}",c,error,sizeof(error)));
    assert(serializePoolDriverConfig(c,encoded,sizeof(encoded)));
    assert(parsePoolDriverConfig(encoded,decoded,error,sizeof(error)) && decoded.serial.line.baud==19200);
    const char* pump = R"({"kind":3,"unit":3,"minimum":1200,"maximum":2900,"startup":2400,
      "serial":{"protocol":1,"baud":1200,"address":170,"control":1,"raw_step":50,"raw_rounding":1,
      "stop_value":1,"running_source":1,"running_threshold":1,"has_feedback":true,
      "setpoint":{"address":3001,"function":208,"layout":1},
      "feedback":{"address":2001,"function":195,"layout":0,"response":1}}})";
    assert(parsePoolDriverConfig(pump,c,error,sizeof(error)));
    assert(serializePoolDriverConfig(c,encoded,sizeof(encoded)));
    assert(parsePoolDriverConfig(encoded,decoded,error,sizeof(error)));
    assert(decoded.capabilities.unit == PoolSetpointUnit::Rpm && decoded.serial.rawStep == 50);
    assert(decoded.serial.control == PoolSerialControl::SetpointOrStop && decoded.serial.runningThreshold == 1);
    assert(decoded.serial.feedback.responseLayout == RegisterResponseLayout::AddressByteCount);
    const char* invalid[] = {
        R"({"kind":3,"serial":{"control":2}})", R"({"kind":3,"serial":{"raw_step":0}})",
        R"({"kind":3,"serial":{"feedback_type":2}})", R"({"kind":3,"serial":{"running_source":2}})",
        R"({"kind":3,"serial":{"raw_rounding":2}})", R"({"kind":3,"serial":{"telemetry_profile":2}})",
        R"({"kind":3,"serial":{"telemetry_stale_ms":-1}})",
        R"({"kind":3,"serial":{"run":{"response":1}}})",
        R"({"kind":3,"serial":{"run_modes":[{"label":"Eco","value":0}]}})",
        R"({"kind":3,"serial":{"run_modes":[{"label":"Eco","value":1},{"label":"Eco","value":2}]}})"
    };
    for (auto json : invalid) assert(!parsePoolDriverConfig(json,c,error,sizeof(error)));
    c = {}; c.capabilities.kind = PoolControlKind::Rs485;
    c.serial.telemetryProfile = PoolTelemetryProfile::HeatPumpPoly;
    c.serial.feedbackType = PoolRegisterValueType::Signed16;
    c.serial.modes.count = POOL_MAX_RUN_MODES;
    for (uint8_t i = 0; i < POOL_MAX_RUN_MODES; ++i) {
        snprintf(c.serial.modes.options[i].label, sizeof(c.serial.modes.options[i].label), "Heat mode %u \\\"eco\\\"", i);
        c.serial.modes.options[i].value = i + 1;
    }
    assert(serializePoolDriverConfig(c,encoded,sizeof(encoded)));
    assert(parsePoolDriverConfig(encoded,decoded,error,sizeof(error)) && decoded.serial.modes.count == 8);
    assert(decoded.serial.telemetryProfile == PoolTelemetryProfile::HeatPumpPoly);
    char options[512], command[1024];
    assert(buildPoolModeHaTemplates(c.serial.modes, 7, options, sizeof(options), command, sizeof(command)));
    SpiRamJsonDocument doc(4096);
    assert(!deserializeJson(doc, options) && doc.size() == 8);
    assert(strcmp(doc[0].as<const char*>(), c.serial.modes.options[0].label) == 0);
    char wrapped[1100]; snprintf(wrapped, sizeof(wrapped), "{\"cmd_tpl\":\"%s\"}", command);
    assert(!deserializeJson(doc, wrapped));
    assert(strstr(doc["cmd_tpl"].as<const char*>(), "pooldevice.mode"));
    assert(!buildPoolModeHaTemplates(c.serial.modes, 7, options, 2, command, sizeof(command)));
    doc.clear(); auto root = doc.to<JsonObject>();
    PoolTelemetryState telemetry; telemetry.validBlocks = 5; telemetry.raw[0] = 1; telemetry.raw[2] = 0xFFCE;
    writePoolTelemetryJson(root, PoolTelemetryProfile::HeatPumpPoly, telemetry, true);
    assert(root["telemetry"]["high_pressure"].as<bool>() && !root["telemetry"]["low_pressure"].as<bool>());
    assert(root["telemetry"]["compressor_out"].as<float>() == -5);
    assert(root["telemetry"]["voltage"].isNull());
    assert(!doc.overflowed());
    for (const char* path : {"docs/examples/rs485/aquagem-candidate.json", "docs/examples/rs485/heatpump-poly-candidate.json"}) {
        std::ifstream input(path);
        assert(input.good());
        const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        assert(parsePoolDriverConfig(json.c_str(), c, error, sizeof(error)));
        assert(serializePoolDriverConfig(c, encoded, sizeof(encoded)));
        assert(parsePoolDriverConfig(encoded, decoded, error, sizeof(error)));
    }

    for (uint8_t model : {1,2}) {
        const std::string json = "{\"kind\":3,\"serial\":{\"model\":" + std::to_string(model) + "}}";
        assert(parsePoolDriverConfig(json.c_str(),c,error,sizeof(error)));
        assert(uint8_t(c.serial.model) == model);
        assert(serializePoolDriverConfig(c,encoded,sizeof(encoded)));
        assert(!strstr(encoded,"function") && !strstr(encoded,"baud") && !strstr(encoded,"maximum"));
        assert(parsePoolDriverConfig(encoded,decoded,error,sizeof(error)));
        assert(decoded.serial.model == c.serial.model && decoded.serial.setpoint.function == c.serial.setpoint.function);
    }
    for (auto json : {R"({"kind":3,"serial":{"model":0}})", R"({"kind":3,"serial":{"model":3}})",
                     R"({"kind":3,"serial":{"model":1,"baud":9600}})"})
        assert(!parsePoolDriverConfig(json,c,error,sizeof(error)));
    puts("pool config tests passed");
}

#include "Modules/IOModule/ValueConfig.h"
#include "Core/Values/ValueFormat.h"
#include "Core/RuntimeUi.h"
#include <cassert>
#include <cstring>
#include <cfloat>
#include <cstdio>

static void candidateTests() {
    ConfigStore store;
    ValueConfig config;
    config.registerConfig(store, 1);
    ValueRegistry registry;
    assert(registry.begin());
    assert(registry.define(1, {}));
    assert(registry.define(33, {ValueType::UInt64, AggregationMode::Counter}));
    assert(registry.define(32, {ValueType::Bool}));
    auto source = [&](ValueId id, ValueType& type) {
        ValueSnapshot sample; ValueMetadata metadata;
        if (!registry.read(id, sample, &metadata)) return false;
        type = metadata.type; return true;
    };
    auto validate = [&](const char* json) {
        StaticJsonDocument<2048> doc;
        assert(!deserializeJson(doc, json));
        return config.validateCandidate(ConfigCandidate(doc.as<JsonObjectConst>()), source);
    };
    assert(!validate(R"({"io/value/v00":{"enabled":true,"expr":"v00+1"}})"));
    assert(!validate(R"({"io/value/v00":{"enabled":true,"expr":"v01"},"io/value/v01":{"enabled":true,"expr":"v00"}})"));
    assert(!validate(R"({"io/value/v00":{"enabled":true,"expr":"a02"}})"));
    assert(!validate(R"({"io/value/v00":{"enabled":true,"expr":"i00.count"}})"));
    assert(!validate(R"({"io/value/v00":{"enabled":true,"expr":"v01"}})"));
    assert(!validate(R"({"io/value/v00":{"precision":7}})"));
    assert(!validate(R"({"io/value/v00":{"aggregation":255}})"));
    assert(!validate(R"({"io/value/v00":{"k3":"abc"}})"));
    assert(!validate(R"({"io/value/v00":{"k3":1e309}})"));
    // The whole candidate, including a forward reference and its activation, is checked together.
    assert(validate(R"({"io/value/v00":{"enabled":true,"expr":"v01+k3","k3":2},"io/value/v01":{"enabled":true,"expr":"i01.count"}})"));
    assert(!config.definitions[0].enabled && !strcmp(config.definitions[0].expr, "0"));
    assert(config.definitions[0].parameters[3] == 0 && registry.used() == 3);
    config.definitions[0].enabled = true;
    strcpy(config.definitions[0].expr, "v00");
    assert(!config.resolve(registry) && registry.used() == 3);
    ValueNumber physical; physical.f = 7;
    assert(registry.write(1, physical, 50));
    ValueSnapshot physicalSample;
    assert(registry.read(1, physicalSample) && physicalSample.value.f == 7);
    strcpy(config.definitions[0].expr, "v01");
    config.definitions[1].enabled = true;
    strcpy(config.definitions[1].expr, "a01");
    assert(!validate(R"({"io/value/v01":{"enabled":false}})"));
    assert(validate(R"({"io/value/v00":{"enabled":false},"io/value/v01":{"enabled":false}})"));
    // Native setters see the same constraints, before replacement of the pointed-to value.
    const bool disabled = false;
    assert(!config.validateCandidate(ConfigCandidate(&config.definitions[1].enabled, &disabled), source));
    assert(config.resolve(registry));
    ValueNumber analog; analog.f = 12;
    assert(registry.write(1, analog, 100));
    ValueSnapshot snapshot; assert(registry.read(96, snapshot));
    assert(snapshot.value.d == 12 && snapshot.quality == ValueQuality::Valid);
}

static void presentationTests() {
    ValueRegistry registry; assert(registry.begin());
    const double parameters[4]{};
    const char* expressions[] = {"1234.567891", "16777217", "1e50", "1e308", "-1e308"};
    for (uint8_t i = 0; i < 5; ++i) {
        ValueExpression::Program program;
        assert(ValueExpression::compile(expressions[i], program));
        assert(registry.defineProgram(96+i, program, parameters, AggregationMode::Gauge, "L", 6));
        ValueSnapshot snapshot; ValueMetadata metadata;
        assert(registry.read(96+i, snapshot, &metadata));
        char json[180], display[40];
        assert(formatValueSnapshot(json, sizeof(json), 96+i, snapshot, metadata));
        StaticJsonDocument<256> parsed;
        assert(!deserializeJson(parsed, static_cast<const char*>(json)));
        // ArduinoJson's fast decimal parser need not be correctly rounded.
        // Check JSON validity and relative accuracy, then exact text round-trip with strtod.
        const double parsedValue = parsed["value"].as<double>();
        assert(isfinite(parsedValue) && fabs(parsedValue / snapshot.value.d - 1) <= 4 * DBL_EPSILON);
        assert(formatValueNumber(display, sizeof(display), snapshot.value.d, 6));
        assert(strtod(display, nullptr) == snapshot.value.d);
        const char* jsonNumber = strstr(json, "\"value\":");
        assert(jsonNumber && strtod(jsonNumber + 8, nullptr) == snapshot.value.d);
        if (i == 0) assert(!strcmp(display, "1234.567891"));
        uint8_t bytes[32]{};
        RuntimeUiBinaryWriter writer(bytes, sizeof(bytes));
        assert(writer.writeF64(2280+i, snapshot.value.d));
        assert(writer.recordCount() == 1 && writer.length() == 11);
        assert(bytes[2] == uint8_t(RuntimeUiWireType::Float64));
        uint64_t bits = 0;
        for (unsigned b = 0; b < 8; ++b) bits |= uint64_t(bytes[3+b]) << (8*b);
        double decoded; memcpy(&decoded, &bits, sizeof(decoded));
        assert(decoded == snapshot.value.d);
        snapshot.quality = ValueQuality::Invalid;
        assert(formatValueSnapshot(json, sizeof(json), 96+i, snapshot, metadata));
        assert(!deserializeJson(parsed, static_cast<const char*>(json))); assert(parsed["value"].isNull());
    }
    char out[40];
    assert(formatValueNumber(out, sizeof(out), DBL_MAX, 6));
    assert(strtod(out, nullptr) == DBL_MAX);
    assert(!formatValueNumber(out, sizeof(out), NAN, 6));
    assert(!formatValueNumber(out, 2, 123.456, 6) && !out[0]);
    assert(roundToPrecision(12.3456, 2) == 12.35);
}
int main() { candidateTests(); presentationTests(); puts("candidate graph and Value -> JSON/display/Float64 pipeline OK"); }

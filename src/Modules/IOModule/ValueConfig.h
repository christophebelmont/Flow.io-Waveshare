#pragma once
#include "Core/ConfigStore.h"
#include "Core/ConfigDoubleAccess.h"
#include "Core/Values/ValueRegistry.h"
#include <esp_heap_caps.h>
#include <new>
#include <stdio.h>

/** Persistent authoring definitions. Expressions/topology are immutable until reboot. */
struct ValueConfig {
    struct Definition {
        bool enabled = false;
        char expr[ValueExpression::TextCapacity] = "0";
        double parameters[ValueExpression::ParamCount] = {1, 0, 0, 0};
        uint8_t mode = 0;
        int32_t precision = 1;
        char name[64]{};
        char unit[UnitTextCapacity]{};
        char path[20]{}, keys[10][12]{};
        ConfigVariable<char, 0> nameVar{}, exprVar{}, unitVar{};
        ConfigVariable<bool, 0> enabledVar{};
        ConfigVariable<double, 0> parameterVars[ValueExpression::ParamCount]{};
        ConfigVariable<uint8_t, 0> modeVar{};
        ConfigVariable<int32_t, 0> precisionVar{};
    } definitions[ValueIds::DerivedCapacity];

    static bool validateExpression(const char* text) {
        ValueExpression::Program program;
        return ValueExpression::compile(text, program);
    }
    void registerConfig(ConfigStore& cfg, uint8_t moduleId) {
        static constexpr const char* parameterNames[] = {"k0", "k1", "k2", "k3"};
        for (uint8_t i = 0; i < ValueIds::DerivedCapacity; ++i) {
            auto& d = definitions[i];
            snprintf(d.path, sizeof(d.path), "io/value/v%02u", unsigned(i));
            for (uint8_t key = 0; key < 10; ++key)
                snprintf(d.keys[key], sizeof(d.keys[key]), "vexpr%02u_%u", unsigned(i), unsigned(key));
            d.exprVar = {d.keys[0], "expr", d.path, ConfigType::CharArray, d.expr, ConfigPersistence::Persistent, sizeof(d.expr)};
            d.exprVar.validateText = validateExpression;
            d.modeVar = {d.keys[1], "aggregation", d.path, ConfigType::UInt8, &d.mode, ConfigPersistence::Persistent, 0};
            d.enabledVar = {d.keys[2], "enabled", d.path, ConfigType::Bool, &d.enabled, ConfigPersistence::Persistent, 0};
            d.nameVar = {d.keys[3], "name", d.path, ConfigType::CharArray, d.name, ConfigPersistence::Persistent, sizeof(d.name)};
            d.unitVar = {d.keys[8], "unit", d.path, ConfigType::CharArray, d.unit, ConfigPersistence::Persistent, sizeof(d.unit)};
            d.precisionVar = {d.keys[9], "precision", d.path, ConfigType::Int32, &d.precision, ConfigPersistence::Persistent, 0};
            const uint8_t branch = 160 + i;
            cfg.registerVar(d.nameVar, moduleId, branch);
            cfg.registerVar(d.unitVar, moduleId, branch);
            cfg.registerVar(d.precisionVar, moduleId, branch);
            cfg.registerVar(d.enabledVar, moduleId, branch);
            cfg.registerVar(d.exprVar, moduleId, branch);
            cfg.registerVar(d.modeVar, moduleId, branch);
            for (uint8_t k = 0; k < ValueExpression::ParamCount; ++k) {
                d.parameterVars[k] = {d.keys[4+k], parameterNames[k], d.path, ConfigType::Double,
                    &d.parameters[k], ConfigPersistence::Persistent, 0};
                cfg.registerVar(d.parameterVars[k], moduleId, branch);
            }
        }
    }
    bool resolve(ValueRegistry& registry) {
        // Boot-only scratch, released on every exit. Keep the task stack bounded even
        // for a chain of all definitions; executable programs live in the registry.
        struct Scratch {
            ValueExpression::Program programs[ValueIds::DerivedCapacity]{};
            bool resolved[ValueIds::DerivedCapacity]{};
            uint8_t order[ValueIds::DerivedCapacity]{};
            ~Scratch() = default;
        };
        void* memory = heap_caps_malloc(sizeof(Scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!memory) memory = heap_caps_malloc(sizeof(Scratch), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!memory) return false;
        auto* scratch = new (memory) Scratch{};
        struct Cleanup {
            Scratch* scratch;
            ~Cleanup() { scratch->~Scratch(); heap_caps_free(scratch); }
        } cleanup{scratch};
        uint8_t count = 0, enabled = 0;
        for (uint8_t i = 0; i < ValueIds::DerivedCapacity; ++i) {
            const auto& d = definitions[i];
            if (!d.enabled) continue;
            ++enabled;
            if (d.mode > uint8_t(AggregationMode::Counter) || !ValueExpression::compile(d.expr, scratch->programs[i])) return false;
            for (double parameter : d.parameters) if (!isfinite(parameter)) return false;
        }
        for (uint8_t pass = 0; pass < ValueIds::DerivedCapacity; ++pass) {
            bool progress = false;
            for (uint8_t i = 0; i < ValueIds::DerivedCapacity; ++i) {
                if (!definitions[i].enabled || scratch->resolved[i]) continue;
                const auto& p = scratch->programs[i];
                bool ready = true;
                for (uint8_t dep = 0; dep < p.dependencyCount; ++dep) {
                    const auto id = p.dependencies[dep];
                    if (id >= ValueIds::Derived) ready &= scratch->resolved[id - ValueIds::Derived];
                    else {
                        ValueSnapshot source;
                        if (!registry.read(id, source)) return false;
                    }
                }
                if (!ready) continue;
                scratch->resolved[i] = true; scratch->order[count++] = i; progress = true;
            }
            if (!progress) break;
        }
        // Reject missing/disabled sources and cycles before installing any program.
        if (count != enabled) return false;
        for (uint8_t position = 0; position < count; ++position) {
            const auto i = scratch->order[position];
            const auto& d = definitions[i];
            int8_t precision = (d.precision < 0) ? 0
                : (d.precision > VALUE_PRECISION_MAX ? VALUE_PRECISION_MAX : (int8_t)d.precision);
            if (!registry.defineProgram(ValueIds::Derived + i, scratch->programs[i], d.parameters,
                                        static_cast<AggregationMode>(d.mode), d.unit, precision)) return false;
            active_[i] = true;
        }
        return true;
    }
    void applyParameters(ValueRegistry& registry, uint64_t timestampMs = 0) const {
        for (uint8_t i = 0; i < ValueIds::DerivedCapacity; ++i) {
            if (!active_[i]) continue;
            double parameters[ValueExpression::ParamCount];
            ConfigDoubleAccess::copy(definitions[i].parameters, parameters, ValueExpression::ParamCount);
            registry.setParameters(ValueIds::Derived + i, parameters, timestampMs);
        }
    }
private:
    bool active_[ValueIds::DerivedCapacity]{};
};

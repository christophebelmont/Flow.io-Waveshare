#pragma once
#include "Core/ConfigStore.h"
#include "Core/ConfigDoubleAccess.h"
#include "Core/ConfigCandidate.h"
#include <memory>
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
    bool affectedBy(const ConfigCandidate& candidate) const {
        for (const auto& d : definitions) {
            if (candidate.contains(d.enabledVar) || candidate.contains(d.exprVar) ||
                candidate.contains(d.modeVar) || candidate.contains(d.precisionVar) ||
                candidate.contains(d.nameVar) || candidate.contains(d.unitVar)) return true;
            for (const auto& parameter : d.parameterVars) if (candidate.contains(parameter)) return true;
        }
        return false;
    }
    template<class ReadSource>
    bool validateCandidate(const ConfigCandidate& candidate, ReadSource readSource) const {
        return bool(makePlan_(candidate, readSource));
    }
    bool resolve(ValueRegistry& registry) {
        auto plan = makePlan_(ConfigCandidate{}, [&registry](ValueId id, ValueType& type) {
            ValueSnapshot snapshot;
            ValueMetadata metadata;
            if (!registry.read(id, snapshot, &metadata)) return false;
            type = metadata.type;
            return true;
        });
        if (!plan) return false;
        for (uint8_t position = 0; position < plan->count; ++position) {
            const auto i = plan->order[position];
            const auto& d = plan->definitions[i];
            if (!registry.defineProgram(ValueIds::Derived + i, plan->programs[i], d.parameters,
                                        static_cast<AggregationMode>(d.mode), d.unit, d.precision)) return false;
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
    struct Settings {
        bool enabled = false;
        char expr[ValueExpression::TextCapacity]{};
        double parameters[ValueExpression::ParamCount]{};
        uint8_t mode = 0;
        int32_t precision = 1;
        char name[64]{}, unit[UnitTextCapacity]{};
    };
    struct Plan {
        Settings definitions[ValueIds::DerivedCapacity]{};
        ValueExpression::Program programs[ValueIds::DerivedCapacity]{};
        bool resolved[ValueIds::DerivedCapacity]{};
        uint8_t order[ValueIds::DerivedCapacity]{}, count = 0;
    };
    struct FreePlan {
        void operator()(Plan* plan) const { if (plan) { plan->~Plan(); heap_caps_free(plan); } }
    };
    using PlanPtr = std::unique_ptr<Plan, FreePlan>;
    template<class ReadSource>
    PlanPtr makePlan_(const ConfigCandidate& candidate, ReadSource readSource) const {
        void* memory = heap_caps_malloc(sizeof(Plan), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!memory) memory = heap_caps_malloc(sizeof(Plan), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!memory) return {};
        PlanPtr plan(new (memory) Plan{});
        uint8_t enabled = 0;
        for (uint8_t i = 0; i < ValueIds::DerivedCapacity; ++i) {
            const auto& current = definitions[i];
            auto& next = plan->definitions[i];
            if (!candidate.read(current.enabledVar, current.enabled, next.enabled) ||
                !candidate.read(current.exprVar, current.expr, next.expr) ||
                !candidate.read(current.modeVar, current.mode, next.mode) ||
                !candidate.read(current.precisionVar, current.precision, next.precision) ||
                !candidate.read(current.nameVar, current.name, next.name) ||
                !candidate.read(current.unitVar, current.unit, next.unit) ||
                next.mode > uint8_t(AggregationMode::Counter) || next.precision < 0 ||
                next.precision > VALUE_PRECISION_MAX) return {};
            double parameters[ValueExpression::ParamCount];
            ConfigDoubleAccess::copy(current.parameters, parameters, ValueExpression::ParamCount);
            for (uint8_t k = 0; k < ValueExpression::ParamCount; ++k)
                if (!candidate.read(current.parameterVars[k], parameters[k], next.parameters[k])) return {};
            if (!next.enabled) continue;
            ++enabled;
            if (!ValueExpression::compile(next.expr, plan->programs[i])) return {};
        }
        for (uint8_t pass = 0; pass < ValueIds::DerivedCapacity; ++pass) {
            bool progress = false;
            for (uint8_t i = 0; i < ValueIds::DerivedCapacity; ++i) {
                if (!plan->definitions[i].enabled || plan->resolved[i]) continue;
                const auto& program = plan->programs[i];
                bool ready = true;
                for (uint8_t dep = 0; dep < program.dependencyCount; ++dep) {
                    const auto id = program.dependencies[dep];
                    if (id >= ValueIds::Derived) ready &= plan->resolved[id - ValueIds::Derived];
                    else {
                        ValueType type;
                        if (!readSource(id, type) || !ValueExpression::validSourceType(id, type)) return {};
                    }
                }
                if (!ready) continue;
                plan->resolved[i] = true;
                plan->order[plan->count++] = i;
                progress = true;
            }
            if (!progress) break;
        }
        if (plan->count != enabled) return {};
        return plan;
    }
    bool active_[ValueIds::DerivedCapacity]{};
};

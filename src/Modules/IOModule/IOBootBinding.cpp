#include "IOModule.h"
#include "IOConfigDescriptorStorage.h"
#include <cmath>
#include "Core/Values/InternalAnalogSources.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

IOPortPresence IOModule::portPresence_(PhysicalPortId port, uint8_t kind) const
{
    if (port == IO_PORT_INVALID) return IOPortPresence::Unconfigured;
    const auto* spec = bindingPortSpec_(port);
    if (!spec) return IOPortPresence::Unsupported;
    uint8_t source, channel, backend, pin;
    IOExpanderId expander;
    bool pcf, tca, mcp;
    if (kind == IO_KIND_ANALOG_IN) {
        if (!resolveAnalogBinding_(port, source, channel, backend)) return IOPortPresence::Invalid;
        if (!analogSourceDriverEnabled_(source)) return IOPortPresence::Disabled;
        return analogPresence_[source];
    }
    const bool compatible = kind == IO_KIND_DIGITAL_IN
        ? resolveDigitalInputBinding_(port, pin, backend, channel, expander)
        : resolveDigitalOutputBinding_(port, pin, backend, channel, expander, pcf, tca, mcp);
    if (!compatible) return IOPortPresence::Invalid;
    // GPIO availability describes the board pin, not the presence of a wired sensor.
    if (backend == IO_BACKEND_GPIO) return IOPortPresence::Present;
    if (expander >= IO_MAX_EXPANDERS) return IOPortPresence::Invalid;
    if (!expanderEnabled_(expander)) return IOPortPresence::Disabled;
    return expanderPresence_[expander];
}

void IOModule::resolveBootBindings_()
{
    if (bindingsResolved_) return;
    for (uint8_t i = 0; i < MAX_ANALOG_ENDPOINTS; ++i) {
        auto& slot = analogSlots_[i];
        if (!slot.used) continue;
        if (analogCfg_[i].sourceValue != VALUE_INVALID) continue;
        const auto& config = analogCfg_[i].binding;
        slot.binding = resolveIOBinding(config, portPresence_(config.primaryPort, IO_KIND_ANALOG_IN),
                                        portPresence_(config.secondaryPort, IO_KIND_ANALOG_IN));
        LOGI("Binding a%02u primary=%u secondary=%u selected=%u reason=%s", i,
             config.primaryPort, config.secondaryPort, slot.binding.port, ioBindingReasonName(slot.binding.reason));
    }
    for (uint8_t i = 0; i < MAX_DIGITAL_SLOTS; ++i) {
        auto& slot = digitalSlots_[i];
        if (!slot.used) continue;
        const bool output = slot.kind == DIGITAL_SLOT_OUTPUT;
        const auto& config = output ? digitalCfg_[slot.logicalIdx].binding : digitalInCfg_[slot.logicalIdx].binding;
        const uint8_t kind = output ? IO_KIND_DIGITAL_OUT : IO_KIND_DIGITAL_IN;
        slot.binding = resolveIOBinding(config, portPresence_(config.primaryPort, kind),
                                        portPresence_(config.secondaryPort, kind));
        // Unsupported input capabilities are configuration errors, not board variants.
        if (!output) {
            const auto& input = digitalInCfg_[slot.logicalIdx];
            for (PhysicalPortId port : {config.primaryPort, config.secondaryPort}) {
                const auto* spec = bindingPortSpec_(port);
                if (spec && spec->kind == IO_PORT_KIND_MCP23017_INPUT &&
                    (input.mode == IO_DIGITAL_INPUT_COUNTER || input.pullMode == IO_PULL_DOWN)) {
                    slot.binding.port = IO_PORT_INVALID;
                    slot.binding.selection = IOBindingSelection::None;
                    slot.binding.reason = IOBindingReason::InvalidConfiguration;
                }
            }
        }
        LOGI("Binding %c%02u primary=%u secondary=%u selected=%u reason=%s", output ? 'd' : 'i', slot.logicalIdx,
             config.primaryPort, config.secondaryPort, slot.binding.port, ioBindingReasonName(slot.binding.reason));
    }
    bindingsResolved_ = true;
}

void IOModule::validateResolvedResources_()
{
    bool conflict[MAX_DIGITAL_SLOTS]{};
    for (uint8_t i = 0; i < MAX_DIGITAL_SLOTS; ++i) {
        if (!digitalSlots_[i].used) continue;
        const auto* a = bindingPortSpec_(digitalSlots_[i].binding.port);
        if (!a) continue;
        for (uint8_t j = 0; j < i; ++j) {
            if (!digitalSlots_[j].used) continue;
            const auto* b = bindingPortSpec_(digitalSlots_[j].binding.port);
            if (!b) continue;
            const bool gpioA = a->kind == IO_PORT_KIND_GPIO_INPUT || a->kind == IO_PORT_KIND_GPIO_OUTPUT;
            const bool gpioB = b->kind == IO_PORT_KIND_GPIO_INPUT || b->kind == IO_PORT_KIND_GPIO_OUTPUT;
            if (a->portId == b->portId || (gpioA && gpioB && a->channel == b->channel)) {
                conflict[i] = conflict[j] = true;
            }
        }
    }
    // Reject all contenders before any pin is initialized, independently of slot order.
    for (uint8_t i = 0; i < MAX_DIGITAL_SLOTS; ++i) {
        if (!conflict[i]) continue;
        auto& binding = digitalSlots_[i].binding;
        LOGE("Binding resource conflict port=%u", binding.port);
        binding.port = IO_PORT_INVALID;
        binding.selection = IOBindingSelection::None;
        binding.reason = IOBindingReason::ResourceConflict;
    }
}

bool IOModule::validateBindingConfig_(const ConfigCandidate& candidate) const
{
    if (!configDescriptors_) return false;
    const auto validate = [&](const auto& vars, const IOBindingConfig& current, uint8_t kind) {
        IOBindingConfig proposed;
        if (!candidate.read(vars.bindingVar, current.primaryPort, proposed.primaryPort) ||
            !candidate.read(vars.secondaryBindingVar, current.secondaryPort, proposed.secondaryPort)) return false;
        if (proposed.primaryPort && proposed.primaryPort == proposed.secondaryPort) return false;
        for (PhysicalPortId port : {proposed.primaryPort, proposed.secondaryPort}) {
            if (!port) continue;
            // Existing unsupported IDs remain diagnosable; newly submitted IDs must exist.
            if (!bindingPortSpec_(port)) return false;
            uint8_t source, channel, backend, pin;
            IOExpanderId expander;
            bool pcf, tca, mcp;
            const bool compatible = kind == IO_KIND_ANALOG_IN ? resolveAnalogBinding_(port, source, channel, backend) :
                kind == IO_KIND_DIGITAL_IN ? resolveDigitalInputBinding_(port, pin, backend, channel, expander) :
                resolveDigitalOutputBinding_(port, pin, backend, channel, expander, pcf, tca, mcp);
            if (!compatible) return false;
        }
        return true;
    };
    for (uint8_t i = 0; i < ANALOG_CFG_SLOTS; ++i) {
        const auto& vars = configDescriptors_->analog[i];
        const auto& config = analogCfg_[i];
        ValueId source;
        if (!candidate.read(vars.sourceValueVar, config.sourceValue, source) ||
            (source != VALUE_INVALID && !InternalAnalogSources::contains(source))) return false;
        if ((candidate.contains(vars.bindingVar) || candidate.contains(vars.secondaryBindingVar)) &&
            !validate(vars, config.binding, IO_KIND_ANALOG_IN)) return false;
        float c0, c1, secondaryC0, secondaryC1;
        if (!candidate.read(vars.c0Var, config.c0, c0) || !candidate.read(vars.c1Var, config.c1, c1) ||
            !candidate.read(vars.secondaryC0Var, config.secondaryC0, secondaryC0) ||
            !candidate.read(vars.secondaryC1Var, config.secondaryC1, secondaryC1) ||
            !std::isfinite(c0) || !std::isfinite(c1) || !std::isfinite(secondaryC0) || !std::isfinite(secondaryC1)) return false;
    }
    for (uint8_t i = 0; i < DIGITAL_INPUT_CFG_SLOTS; ++i) {
        const auto& vars = configDescriptors_->digitalInputs[i];
        const auto& config = digitalInCfg_[i];
        if (!candidate.contains(vars.bindingVar) && !candidate.contains(vars.secondaryBindingVar) &&
            !candidate.contains(vars.modeVar) && !candidate.contains(vars.pullModeVar)) continue;
        if (!validate(vars, config.binding, IO_KIND_DIGITAL_IN)) return false;
        PhysicalPortId primary, secondary;
        uint8_t mode, pull;
        if (!candidate.read(vars.bindingVar, config.binding.primaryPort, primary) ||
            !candidate.read(vars.secondaryBindingVar, config.binding.secondaryPort, secondary) ||
            !candidate.read(vars.modeVar, config.mode, mode) || !candidate.read(vars.pullModeVar, config.pullMode, pull)) return false;
        for (PhysicalPortId port : {primary, secondary}) {
            const auto* spec = bindingPortSpec_(port);
            if (spec && spec->kind == IO_PORT_KIND_MCP23017_INPUT && (mode == IO_DIGITAL_INPUT_COUNTER || pull == IO_PULL_DOWN)) return false;
        }
    }
    for (uint8_t i = 0; i < DIGITAL_CFG_SLOTS; ++i) {
        const auto& vars = configDescriptors_->digitalOutputs[i];
        if ((candidate.contains(vars.bindingVar) || candidate.contains(vars.secondaryBindingVar)) &&
            !validate(vars, digitalCfg_[i].binding, IO_KIND_DIGITAL_OUT)) return false;
    }
    return true;
}

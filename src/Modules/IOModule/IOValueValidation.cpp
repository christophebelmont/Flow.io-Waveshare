#include "IOModule.h"
#include "IOConfigDescriptorStorage.h"
#include "ValueConfig.h"
#include "Core/Values/InternalAnalogSources.h"
#include "Core/Values/PoolDeviceValues.h"

bool IOModule::validateValueConfig_(const ConfigCandidate& candidate) const
{
    if (!valueConfig_ || !configDescriptors_) return false;
    bool affected = valueConfig_->affectedBy(candidate) || candidate.contains(enabledVar_) ||
        candidate.contains(sht40EnabledVar_) || candidate.contains(bmp280EnabledVar_) ||
        candidate.contains(bme680EnabledVar_) || candidate.contains(ina226EnabledVar_);
    for (uint8_t i = 0; i < MAX_ANALOG_ENDPOINTS; ++i)
        affected |= candidate.contains(configDescriptors_->analog[i].sourceValueVar) || candidate.contains(configDescriptors_->analog[i].bindingVar) ||
                    candidate.contains(configDescriptors_->analog[i].secondaryBindingVar);
    for (uint8_t i = 0; i < MAX_DIGITAL_INPUTS; ++i) {
        const auto& vars = configDescriptors_->digitalInputs[i];
        affected |= candidate.contains(vars.bindingVar) || candidate.contains(vars.secondaryBindingVar) || candidate.contains(vars.modeVar) || candidate.contains(vars.c0Var);
    }
    if (!affected) return true;
    bool ioEnabled;
    if (!candidate.read(enabledVar_, cfgData_.enabled, ioEnabled)) return false;
    return valueConfig_->validateCandidate(candidate, [&](ValueId id, ValueType& type) {
        if (ValueIds::isEquipment(id)) {
            ValueSnapshot sample{}; ValueMetadata metadata{};
            if (!dataStore_ || !dataStore_->values.read(id, sample, &metadata)) return false;
            type = metadata.type;
            return metadata.usable;
        }
        if (PoolDeviceValues::isUptime(id)) { type = ValueType::UInt64; return true; }
        if (InternalAnalogSources::contains(id)) { type = ValueType::Float; return true; }
        if (!ioEnabled) return false;
        if (id < ValueIds::Digital) {
            if (id >= MAX_ANALOG_ENDPOINTS || !analogSlots_[id].used) return false;
            const auto& vars = configDescriptors_->analog[id];
            const auto& config = analogCfg_[id];
            ValueId source;
            if (!candidate.read(vars.sourceValueVar, config.sourceValue, source)) return false;
            if (source != VALUE_INVALID) {
                type = ValueType::Float;
                return InternalAnalogSources::contains(source);
            }
            PhysicalPortId primary, secondary;
            if (!candidate.read(vars.bindingVar, config.binding.primaryPort, primary) ||
                !candidate.read(vars.secondaryBindingVar, config.binding.secondaryPort, secondary)) return false;
            type = ValueType::Float;
            for (PhysicalPortId port : {primary, secondary}) {
                uint8_t source, channel, backend;
                if (!resolveAnalogBinding_(port, source, channel, backend)) continue;
                bool enabled = true;
                switch (source) {
                    case IO_SRC_SHT40:
                        if (!candidate.read(sht40EnabledVar_, cfgData_.sht40Enabled, enabled)) return false;
                        break;
                    case IO_SRC_BMP280:
                        if (!candidate.read(bmp280EnabledVar_, cfgData_.bmp280Enabled, enabled)) return false;
                        break;
                    case IO_SRC_BME680:
                        if (!candidate.read(bme680EnabledVar_, cfgData_.bme680Enabled, enabled)) return false;
                        break;
                    case IO_SRC_INA226:
                        if (!candidate.read(ina226EnabledVar_, cfgData_.ina226Enabled, enabled)) return false;
                        break;
                    default: break;
                }
                if (enabled) return true;
            }
            return false;
        }
        const bool state = ValueIds::isDigitalState(id);
        const auto slot = state ? uint8_t(id - ValueIds::DigitalState) : uint8_t((id - ValueIds::Digital) % 16);
        if (slot >= MAX_DIGITAL_INPUTS) return false;
        uint8_t runtimeSlot;
        if (!findDigitalSlotByLogical_(DIGITAL_SLOT_INPUT, slot, runtimeSlot)) return false;
        const auto& vars = configDescriptors_->digitalInputs[slot];
        const auto& current = digitalInCfg_[slot];
        PhysicalPortId primary, secondary;
        uint8_t mode, pin, backend, channel;
        IOExpanderId expander;
        float scale;
        if (!candidate.read(vars.bindingVar, current.binding.primaryPort, primary) ||
            !candidate.read(vars.secondaryBindingVar, current.binding.secondaryPort, secondary) ||
            !candidate.read(vars.modeVar, current.mode, mode) ||
            !candidate.read(vars.c0Var, current.c0, scale) || scale < 0 ||
            mode != (state ? IO_DIGITAL_INPUT_STATE : IO_DIGITAL_INPUT_COUNTER)) return false;
        bool hasCounterPort = false;
        for (PhysicalPortId port : {primary, secondary}) {
            if (!port) continue;
            if (!resolveDigitalInputBinding_(port, pin, backend, channel, expander) || (!state && backend != IO_BACKEND_GPIO)) return false;
            hasCounterPort = true;
        }
        if (!hasCounterPort) return false;
        if (state) { type = ValueType::Bool; return true; }
        type = id < ValueIds::PulseRate ? ValueType::UInt64 :
            (id < ValueIds::Total ? ValueType::Float : ValueType::Double);
        return true;
    });
}

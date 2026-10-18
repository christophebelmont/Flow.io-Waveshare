#include "Modules/IOModule/IOConfigDescriptorStorage.h"
#include "Core/ConfigCandidate.h"
#include "Core/Values/InternalAnalogSources.h"
#include "Core/I2cAddressSelection.h"
#include <cassert>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
uint32_t millis() { return 1; }

struct ConfigStore {
    std::set<std::string> keys;
    bool hasVersion = false, failWrite = false;
    unsigned writes = 0;
    bool readRuntimeBlob(const char*, void* out, size_t, size_t* size) {
        if (!hasVersion) return false;
        *static_cast<uint8_t*>(out) = 1; *size = 1; return true;
    }
    bool persistentKeyExists(const char* key, bool& exists) { exists = keys.count(key); return true; }
    bool persistRegisteredValue(const void*) { ++writes; return !failWrite; }
    bool writeRuntimeBlob(const char*, const void*, size_t) { ++writes; return hasVersion = !failWrite; }
};

struct TestDataStore { ValueRegistry values; };

struct IOModule {
    TestDataStore* dataStore_ = nullptr;
    static constexpr uint8_t MAX_ANALOG_ENDPOINTS = Limits::Io::AnalogConfigSlots;
    static constexpr uint8_t ANALOG_CFG_SLOTS = MAX_ANALOG_ENDPOINTS;
    static constexpr uint8_t DIGITAL_INPUT_CFG_SLOTS = Limits::Io::DigitalInputConfigSlots;
    static constexpr uint8_t DIGITAL_CFG_SLOTS = Limits::Io::DigitalOutputConfigSlots;
    static constexpr uint8_t MAX_DIGITAL_SLOTS = DIGITAL_INPUT_CFG_SLOTS + DIGITAL_CFG_SLOTS;
    static constexpr uint8_t DIGITAL_SLOT_INPUT = 0, DIGITAL_SLOT_OUTPUT = 1;
    struct AnalogSlot { bool used = false; IOAnalogDefinition def{}; IOResolvedBinding binding{}; };
    struct DigitalSlot { bool used = false; uint8_t kind = 0, logicalIdx = 0; IOResolvedBinding binding{}; };
    AnalogSlot analogSlots_[MAX_ANALOG_ENDPOINTS]{};
    DigitalSlot digitalSlots_[MAX_DIGITAL_SLOTS]{};
    IOAnalogSlotConfig analogCfg_[ANALOG_CFG_SLOTS]{};
    IODigitalInputSlotConfig digitalInCfg_[DIGITAL_INPUT_CFG_SLOTS]{};
    IODigitalOutputSlotConfig digitalCfg_[DIGITAL_CFG_SLOTS]{};
    IOConfigDescriptorStorage descriptors{analogCfg_, digitalInCfg_, digitalCfg_};
    IOConfigDescriptorStorage* configDescriptors_ = &descriptors;
    IOPortPresence analogPresence_[IO_SRC_COUNT]{};
    IOPortPresence expanderPresence_[IO_MAX_EXPANDERS]{};
    bool enabled[IO_SRC_COUNT] = {true,true,true,true,true,true,true,true};
    bool expanderEnabled = true;
    bool bindingsResolved_ = false, bindingMigrationPending_ = false;
    uint32_t legacyBindingSlots_ = 0;
    IOBindingPortSpec ports[6] = {
        {100,IO_PORT_KIND_ADS_INTERNAL_SINGLE,0,0,"internal"},
        {110,IO_PORT_KIND_ADS_EXTERNAL_DIFF,0,0,"external"},
        {201,IO_PORT_KIND_GPIO_INPUT,5,0,"in"},
        {301,IO_PORT_KIND_GPIO_OUTPUT,5,0,"out"},
        {220,IO_PORT_KIND_MCP23017_INPUT,0,0,"mcp"},
        {302,IO_PORT_KIND_GPIO_OUTPUT,6,0,"out2"}
    };
    const IOBindingPortSpec* bindingPortSpec_(PhysicalPortId port) const {
        for (const auto& spec : ports) if (port == spec.portId) return &spec;
        return nullptr;
    }
    bool analogSourceDriverEnabled_(uint8_t source) const { return enabled[source]; }
    bool expanderEnabled_(uint8_t) const { return expanderEnabled; }
    bool resolveAnalogBinding_(PhysicalPortId, uint8_t&, uint8_t&, uint8_t&) const;
    bool resolveDigitalInputBinding_(PhysicalPortId, uint8_t&, uint8_t&, uint8_t&, IOExpanderId&) const;
    bool resolveDigitalOutputBinding_(PhysicalPortId, uint8_t&, uint8_t&, uint8_t&, IOExpanderId&, bool&, bool&, bool&) const;
    IOPortPresence portPresence_(PhysicalPortId, uint8_t) const;
    void resolveBootBindings_();
    void validateResolvedResources_();
    bool validateBindingConfig_(const ConfigCandidate&) const;
    bool runtimeReady_ = true, analogPrecisionLastInit_ = true;
    int32_t analogPrecisionLast_[ANALOG_CFG_SLOTS]{};
    uint32_t analogConfigDirtyMask_ = 0;
    bool ensureAnalogPrecisionState_() { return true; }
    void forceAnalogSnapshotPublish_(uint8_t, uint32_t) {}
    int32_t sanitizeAnalogPrecision_(int32_t) const;
    void refreshAnalogConfigState_();
    void prepareBindingMigration_(ConfigStore&);
    void persistBindingMigration_(ConfigStore&);
};

// PRODUCTION_FUNCTIONS

int main()
{
    using P = IOPortPresence;
    using S = IOBindingSelection;
    using R = IOBindingReason;
    const IOBindingConfig choices{110,100};
    assert(resolveIOBinding(choices,P::Present,P::Present).port == 110);
    assert(resolveIOBinding(choices,P::Absent,P::Present).port == 100);
    assert(resolveIOBinding(choices,P::Disabled,P::Present).reason == R::PrimaryDisabled);
    assert(resolveIOBinding(choices,P::Unsupported,P::Present).port == 100);
    assert(resolveIOBinding(choices,P::Absent,P::Absent).reason == R::NoEligiblePort);
    assert(resolveIOBinding(choices,P::DetectionFailed,P::Present).reason == R::DetectionFailed);
    assert(resolveIOBinding(choices,P::Invalid,P::Present).reason == R::InvalidConfiguration);
    assert(resolveIOBinding(choices,P::Present,P::Invalid).reason == R::InvalidConfiguration);
    assert(resolveIOBinding({110,110},P::Present,P::Present).port == 0);
    assert(resolveIOBinding({},P::Unconfigured,P::Unconfigured).reason == R::Unconfigured);
    assert(resolveIOBinding({0,100},P::Unconfigured,P::Present).port == 100);

    IOModule module;
    module.analogSlots_[0].used = true;
    module.analogCfg_[0].binding = choices;
    module.analogPresence_[IO_SRC_ADS_INTERNAL_SINGLE] = P::Present;
    module.analogPresence_[IO_SRC_ADS_EXTERNAL_DIFF] = P::Absent;
    module.resolveBootBindings_();
    assert(module.analogSlots_[0].binding.port == 100);
    assert(module.analogSlots_[0].binding.selection == S::Secondary);
    assert(module.analogCfg_[0].binding.primaryPort == 110);
    module.analogCfg_[0].c0 = 431.03f;
    module.analogCfg_[0].c1 = 0;
    module.analogCfg_[0].secondaryC0 = 129.2f;
    module.analogCfg_[0].secondaryC1 = 384.1f;
    module.refreshAnalogConfigState_();
    assert(module.analogSlots_[0].def.c0 == 129.2f && module.analogSlots_[0].def.c1 == 384.1f);
    module.analogCfg_[0].secondaryC0 = 150;
    module.refreshAnalogConfigState_();
    assert(module.analogSlots_[0].def.c0 == 150);
    module.analogCfg_[0].binding.secondaryPort = 110;
    module.analogCfg_[0].secondaryC0 = 500;
    module.refreshAnalogConfigState_();
    assert(module.analogSlots_[0].def.c0 == 150); // Pending route uses its old calibration until reboot.

    // Neither a reconnected primary, a lost secondary nor a pending config edit changes the boot result.
    module.analogPresence_[IO_SRC_ADS_EXTERNAL_DIFF] = P::Present;
    module.analogPresence_[IO_SRC_ADS_INTERNAL_SINGLE] = P::Absent;
    module.analogCfg_[0].binding.primaryPort = 100;
    module.resolveBootBindings_();
    assert(module.analogSlots_[0].binding.port == 100);
    assert(module.analogSlots_[0].binding.bootConfig.primaryPort == 110);

    IOModule conflict;
    conflict.digitalSlots_[0] = {true, IOModule::DIGITAL_SLOT_INPUT, 0, {}};
    conflict.digitalSlots_[1] = {true, IOModule::DIGITAL_SLOT_OUTPUT, 0, {}};
    conflict.digitalInCfg_[0].binding = {201,0};
    conflict.digitalCfg_[0].binding = {301,302};
    conflict.resolveBootBindings_();
    conflict.validateResolvedResources_();
    for (unsigned i = 0; i < 2; ++i) {
        assert(conflict.digitalSlots_[i].binding.port == 0);
        assert(conflict.digitalSlots_[i].binding.reason == R::ResourceConflict);
    }
    // An input counter cannot silently become an expander state input.
    IOModule input;
    input.digitalSlots_[0] = {true, IOModule::DIGITAL_SLOT_INPUT, 0, {}};
    input.digitalInCfg_[0].binding = {201,220};
    input.digitalInCfg_[0].mode = IO_DIGITAL_INPUT_COUNTER;
    input.resolveBootBindings_();
    assert(input.digitalSlots_[0].binding.reason == R::InvalidConfiguration);

    IOModule validation;
    uint16_t good = 110, bad = 999, wrongDirection = 201;
    auto& config = validation.analogCfg_[0];
    assert(validation.validateBindingConfig_(ConfigCandidate(&config.binding.primaryPort,&good)));
    assert(!validation.validateBindingConfig_(ConfigCandidate(&config.binding.primaryPort,&bad)));
    assert(!validation.validateBindingConfig_(ConfigCandidate(&config.binding.primaryPort,&wrongDirection)));
    config.binding.primaryPort = 110;
    assert(!validation.validateBindingConfig_(ConfigCandidate(&config.binding.secondaryPort,&good)));
    float nan = NAN;
    assert(!validation.validateBindingConfig_(ConfigCandidate(&config.secondaryC0,&nan)));
    // Both candidates are read from one JSON transaction before accepting it.
    StaticJsonDocument<512> patch;
    deserializeJson(patch, R"({"io/input/a00":{"binding_port":100,"secondary_binding_port":110,"secondary_c0":431.03}})");
    assert(validation.validateBindingConfig_(ConfigCandidate(patch.as<JsonObjectConst>())));

    // Internal sources are roots: never resolve a stored physical fallback or accept another AI as source.
    patch.clear();
    deserializeJson(patch, R"({"io/input/a00":{"source_value":115}})");
    assert(validation.validateBindingConfig_(ConfigCandidate(patch.as<JsonObjectConst>())));
    patch["io/input/a00"]["source_value"] = 12;
    assert(!validation.validateBindingConfig_(ConfigCandidate(patch.as<JsonObjectConst>())));
    patch["io/input/a00"]["source_value"] = 144;
    assert(!validation.validateBindingConfig_(ConfigCandidate(patch.as<JsonObjectConst>())));
    patch["io/input/a00"]["source_value"] = 65535;
    assert(validation.validateBindingConfig_(ConfigCandidate(patch.as<JsonObjectConst>())));
    IOModule internal;
    internal.analogSlots_[0].used = true;
    internal.analogCfg_[0].sourceValue = 115;
    internal.analogCfg_[0].binding = choices;
    internal.resolveBootBindings_();
    assert(internal.analogSlots_[0].binding.port == IO_PORT_INVALID);
    assert(internal.analogCfg_[0].binding.primaryPort == choices.primaryPort);

    IOModule legacy;
    legacy.analogSlots_[0].def.legacyDefault = {100,129.2f,384.1f};
    legacy.analogCfg_[0].binding = {110,100};
    legacy.analogCfg_[0].c0 = 431.03f;
    legacy.analogCfg_[0].secondaryC0 = 129.2f;
    ConfigStore persisted;
    persisted.keys.insert("io_a00bp");
    legacy.prepareBindingMigration_(persisted);
    assert(legacy.analogCfg_[0].binding.primaryPort == 100);
    assert(legacy.analogCfg_[0].binding.secondaryPort == 0);
    assert(legacy.analogCfg_[0].c0 == 129.2f);
    // Simulate normal ConfigStore load after applying legacy defaults.
    legacy.analogCfg_[0].c0 = 150.0f;
    persisted.failWrite = true;
    legacy.persistBindingMigration_(persisted);
    assert(!persisted.hasVersion);
    persisted.failWrite = false;
    legacy.persistBindingMigration_(persisted);
    assert(persisted.hasVersion && legacy.analogCfg_[0].c0 == 150.0f);
    IOModule fresh;
    fresh.analogCfg_[0].binding = choices;
    ConfigStore empty;
    fresh.prepareBindingMigration_(empty);
    fresh.persistBindingMigration_(empty);
    assert(fresh.analogCfg_[0].binding.secondaryPort == 100 && empty.writes == 1);
    empty.keys.insert("io_a00bp");
    fresh.prepareBindingMigration_(empty);
    assert(!fresh.bindingMigrationPending_ && fresh.analogCfg_[0].binding.secondaryPort == 100);

    // Bus errors cannot masquerade as absent hardware, including alternate addresses.
    I2cAddressSelection devices[] = {{0x48,0x49,true}};
    resolveI2cAddresses(devices,1,[](uint8_t) { return I2cProbeResult::Error; });
    assert(devices[0].status == I2cAddressStatus::DetectionFailed && devices[0].active == 0);
    resolveI2cAddresses(devices,1,[](uint8_t address) { return address == 0x48 ? I2cProbeResult::Absent : I2cProbeResult::Present; });
    assert(devices[0].active == 0x49);
    // Persistent fields point to separate primary/secondary values and keep old keys.
    assert(!strcmp(validation.descriptors.analog[0].bindingVar.nvsKey,"io_a00bp"));
    assert(validation.descriptors.analog[0].secondaryC0Var.value == &config.secondaryC0);
    assert(validation.descriptors.analog[0].c0Var.value == &config.c0);
}

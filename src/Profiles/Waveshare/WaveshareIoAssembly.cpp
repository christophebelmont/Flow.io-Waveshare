#include "Core/Values/DataValueView.h"
#include "Profiles/Waveshare/WaveshareIoAssembly.h"
#include "Profiles/Waveshare/PoolRoleHaDiscovery.h"
#include "Profiles/Waveshare/WaveshareIoLayout.h"

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <esp_heap_caps.h>

#include "App/AppContext.h"
#include "Board/BoardSpec.h"
#include "Board/BoardSerialMap.h"
#include "Core/MqttTopics.h"
#include "Core/Log.h"
#include "Core/LogModuleIds.h"
#include "Core/Services/Services.h"
#include "Domain/Pool/PoolBehaviors.h"
#include "Domain/Pool/PoolIds.h"
#include "Modules/IOModule/IORuntime.h"
#include "Modules/Network/HAModule/HARuntime.h"
#include "Profiles/Waveshare/WaveshareProfile.h"

#ifndef FLOW_HA_BOOT_TRACE
#define FLOW_HA_BOOT_TRACE 0
#endif

#if FLOW_HA_BOOT_TRACE
#define WAVESHARE_HA_BOOT_TRACE(FMT, ...) Board::SerialMap::logSerial().printf("[HA-BOOT] " FMT "\r\n", ##__VA_ARGS__)
#else
#define WAVESHARE_HA_BOOT_TRACE(FMT, ...) do {} while (0)
#endif

namespace {

using Profiles::Waveshare::ModuleInstances;
namespace FlowIoLayout = Profiles::Waveshare::IoLayout;
static constexpr uint8_t kFlowIoAnalogHaSlots = Limits::Io::MaxAnalogEndpoints;

struct FlowIoDiscoveryHeap {
    char analogValueTpl[kFlowIoAnalogHaSlots][128]{};
    char dataNames[kDataValueCount][64]{};
    char dataUnits[kDataValueCount][UnitTextCapacity]{};
    PoolRoleHaDiscovery::Storage poolRoles{};
};

FlowIoDiscoveryHeap* gDiscoveryHeap = nullptr;
bool gDiscoveryHeapReleaseWaitLogged = false;
bool gOneShotRefreshBypassedLogged = false;

bool ensureDiscoveryHeap()
{
    if (gDiscoveryHeap) return true;
    gDiscoveryHeap = static_cast<FlowIoDiscoveryHeap*>(
        heap_caps_calloc(1, sizeof(FlowIoDiscoveryHeap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (!gDiscoveryHeap) {
        gDiscoveryHeap = static_cast<FlowIoDiscoveryHeap*>(
            heap_caps_calloc(1, sizeof(FlowIoDiscoveryHeap), MALLOC_CAP_8BIT)
        );
    }
    if (gDiscoveryHeap) {
        WAVESHARE_HA_BOOT_TRACE("flow.io discovery heap allocated (%u bytes)", (unsigned)sizeof(FlowIoDiscoveryHeap));
    } else {
        WAVESHARE_HA_BOOT_TRACE("flow.io discovery heap allocation failed (%u bytes)", (unsigned)sizeof(FlowIoDiscoveryHeap));
    }
    return gDiscoveryHeap != nullptr;
}

void releaseDiscoveryHeapIfReady(ModuleInstances& modules)
{
#if FLOW_HA_ONESHOT_DISCOVERY
    if (!gDiscoveryHeap || !modules.ioDataStore) return;
    if (!haAutoconfigPublished(*modules.ioDataStore)) {
        if (!gDiscoveryHeapReleaseWaitLogged) {
            WAVESHARE_HA_BOOT_TRACE("flow.io discovery heap waiting for HA publish completion");
            gDiscoveryHeapReleaseWaitLogged = true;
        }
        return;
    }
    heap_caps_free(gDiscoveryHeap);
    gDiscoveryHeap = nullptr;
    gDiscoveryHeapReleaseWaitLogged = false;
    Board::SerialMap::logSerial().printf(
        "[waveshare] HA discovery released bytes=%u internal_free=%lu internal_largest=%lu psram_free=%lu\r\n",
        unsigned(sizeof(FlowIoDiscoveryHeap)),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    (void)modules;
#endif
}

const DomainSlotPreset* findDomainSlotById(const DomainSpec& domain, DomainSlotId id)
{
    for (uint8_t i = 0; i < domain.domainSlotCount; ++i) {
        const DomainSlotPreset& slot = domain.domainSlots[i];
        if (slot.id == id) return &slot;
    }
    return nullptr;
}

IoSlotId findIoSlotForDomainSlot(const DomainSpec& domain, DomainSlotId id)
{
    for (uint8_t i = 0; i < domain.domainIoSlotBindingCount; ++i) {
        const DomainIoSlotBinding& binding = domain.domainIoSlotBindings[i];
        if (binding.domainSlot == id) return binding.ioSlot;
    }
    return IO_SLOT_INVALID;
}

const PoolDevicePreset* findPoolPresetById(const DomainSpec& domain, PoolDeviceId id)
{
    for (uint8_t i = 0; i < domain.poolDeviceCount; ++i) {
        const PoolDevicePreset& preset = domain.poolDevices[i];
        if (preset.id == id) return &preset;
    }
    return nullptr;
}

uint8_t digitalInputOrdinalFromPort(PhysicalPortId port)
{
    switch (port) {
        case FlowIoLayout::PortGpio5Input: return 2;
        case FlowIoLayout::PortGpio6Input: return 3;
        case FlowIoLayout::PortGpio7Input: return 4;
        case FlowIoLayout::PortGpio8Input: return 5;
        case FlowIoLayout::PortGpio9Input: return 6;
        case FlowIoLayout::PortGpio10Input: return 7;
        case FlowIoLayout::PortGpio11Input: return 8;
        default: return 0;
    }
}

PhysicalPortId digitalInputPortFromOrdinal(uint8_t ordinal)
{
    switch (ordinal) {
        case 2: return FlowIoLayout::PortGpio5Input;
        case 3: return FlowIoLayout::PortGpio6Input;
        case 4: return FlowIoLayout::PortGpio7Input;
        case 5: return FlowIoLayout::PortGpio8Input;
        case 6: return FlowIoLayout::PortGpio9Input;
        case 7: return FlowIoLayout::PortGpio10Input;
        case 8: return FlowIoLayout::PortGpio11Input;
        default: return IO_PORT_INVALID;
    }
}

PhysicalPortId waveshareCompOutputPort(uint8_t idx)
{
    switch (idx) {
        case 0: return FlowIoLayout::PortMcpOutGpb0;
        case 1: return FlowIoLayout::PortMcpOutGpb1;
        case 2: return FlowIoLayout::PortMcpOutGpb2;
        case 3: return FlowIoLayout::PortMcpOutGpb3;
        case 4: return FlowIoLayout::PortMcpOutGpb4;
        case 5: return FlowIoLayout::PortMcpOutGpb5;
        case 6: return FlowIoLayout::PortMcpOutGpb6;
        case 7: return FlowIoLayout::PortMcpOutGpb7;
        default: return IO_PORT_INVALID;
    }
}

PhysicalPortId waveshareMcpInputPort(uint8_t idx)
{
    switch (idx) {
        case 0: return FlowIoLayout::PortMcpInGpa0;
        case 1: return FlowIoLayout::PortMcpInGpa3;
        case 2: return FlowIoLayout::PortMcpInGpa4;
        case 3: return FlowIoLayout::PortMcpInGpa5;
        case 4: return FlowIoLayout::PortMcpInGpa6;
        default: return IO_PORT_INVALID;
    }
}

void requireSetup(bool ok, const char* step)
{
    if (ok) return;
    Log::error((LogModuleId)LogModuleIdValue::Core, "setup failure: %s", step ? step : "unknown");
    if (!Log::hub()) {
        Board::SerialMap::logSerial().printf("Setup failure: %s\r\n", step ? step : "unknown");
    }
    while (true) delay(1000);
}

// Discovery is optional: report failures without stopping hardware startup.
bool reportDiscoveryResult(bool ok, const char* step)
{
    if (!ok) {
        Log::error((LogModuleId)LogModuleIdValue::Core,
                   "HA discovery failed: %s; boot continues, retry after restart", step);
    }
    return ok;
}

void applyAnalogDefaultsForIoSlot(IoSlotId ioSlot, IOAnalogDefinition& def)
{
    const FlowIoLayout::AnalogSlotDefault* spec = FlowIoLayout::analogDefaultForIoSlot(ioSlot);
    if (!spec) return;
    snprintf(def.id, sizeof(def.id), "%s", spec->name);
    def.binding.primaryPort = spec->bindingPort;
    def.binding.secondaryPort = spec->secondaryPort;
    def.legacyDefault = spec->legacyDefault;
    def.secondaryC0 = spec->secondaryC0;
    def.secondaryC1 = spec->secondaryC1;
    def.c0 = spec->c0;
    def.c1 = spec->c1;
    def.precision = spec->precision;
    def.unit = spec->unit;
}

void applyDigitalDefaultsForDomainSlot(DomainSlotId domainSlot, IODigitalInputDefinition& def)
{
    const FlowIoLayout::DigitalInputRoleDefault* spec = FlowIoLayout::digitalInputDefaultForDomainSlot(domainSlot);
    requireSetup(spec != nullptr, "unsupported digital input domain role");
    def.binding.primaryPort = spec->bindingPort;
    def.activeHigh = spec->activeHigh;
    def.pullMode = spec->pullMode;
    def.mode = spec->mode;
    def.edgeMode = spec->edgeMode;
    def.counterDebounceUs = spec->debounceUs;
}

const char* waveshareDigitalInputNameForDomainSlot(DomainSlotId domainSlot)
{
    switch (domainSlot) {
        case PoolIds::SensorPir: return "PIR";
        case PoolIds::SensorPhLevel: return "pH Level";
        case PoolIds::SensorChlorineLevel: return "Chlorine Level";
        case PoolIds::SensorPoolLevel: return "Pool Level";
        case PoolIds::SensorWaterMeter: return "Water Meter";
        default: return nullptr;
    }
}

const char* waveshareDigitalInputNameForLogical(uint8_t logicalIdx)
{
    switch (logicalIdx) {
        case PoolInputSlots::FactoryReset: return "Factory Reset";
        case PoolInputSlots::WaterMeter: return "Water Meter";
        case PoolInputSlots::PoolLevel: return "Pool Level";
        case PoolInputSlots::ChlorineLevel: return "Chlorine Level";
        case PoolInputSlots::PhLevel: return "pH Level";
        case PoolInputSlots::FlowMeter: return "Flow Meter";
        case PoolInputSlots::Spare: return "GPIO10";
        case PoolInputSlots::Pir: return "PIR";
        default: return "DIN";
    }
}

void buildAnalogValueTemplate(const IOModule& ioModule, uint8_t analogIdx, char* out, size_t outLen)
{
    if (!out || outLen == 0) return;
    const int32_t precision = ioModule.analogPrecision(analogIdx);
    snprintf(
        out,
        outLen,
        "{%% if value_json.value is number %%}{{ value_json.value | float | round(%ld) }}{%% else %%}unavailable{%% endif %%}",
        (long)precision
    );
}

void syncDataSensors(ModuleInstances& modules)
{
    if (!modules.ioDataStore || !modules.haService || !modules.haService->addSensor) return;
    if (!reportDiscoveryResult(ensureDiscoveryHeap(), "data discovery heap")) return;
    for (size_t index = 0; index < kDataValueCount; ++index) {
        const auto& d = kDataValues[index];
        ValueSnapshot value{}; ValueMetadata meta{};
        const bool defined = modules.ioDataStore->values.read(d.id, value, &meta) && dataValueCompatible(d, meta);
        if (!modules.ioDataStore->publishHa(d.id)) {
            if (modules.haService->addDiscoveryRemoval) {
                const HADiscoveryRemovalEntry removal{d.kind == DataValueKind::Digital ? "binary_sensor" : "sensor", d.haSuffix};
                reportDiscoveryResult(modules.haService->addDiscoveryRemoval(modules.haService->ctx, &removal), d.haSuffix);
            }
            continue;
        }
        if (!defined) continue;
        auto& name = gDiscoveryHeap->dataNames[index];
        auto& unit = gDiscoveryHeap->dataUnits[index];
        snprintf(name, sizeof(name), "%s", dataValueLabel(d.id, meta));
        snprintf(unit, sizeof(unit), "%s", meta.displayUnit);
        if (d.kind == DataValueKind::Digital) {
            if (!modules.haService->addBinarySensor) continue;
            const HABinarySensorEntry entry{"io", d.haSuffix, name, d.topic,
                "{{ 'True' if value_json.value else 'False' }}", nullptr, nullptr,
                "mdi:electric-switch", nullptr, true,
                "{{ 'online' if value_json.quality == 1 else 'offline' }}", d.id};
            reportDiscoveryResult(modules.haService->addBinarySensor(modules.haService->ctx, &entry), d.haSuffix);
            continue;
        }
        const bool analog = d.kind == DataValueKind::Analog;
        const char* valueTemplate = "{{ value_json.value }}";
        if (analog) {
            buildAnalogValueTemplate(modules.ioModule, d.slot, gDiscoveryHeap->analogValueTpl[d.slot], sizeof(gDiscoveryHeap->analogValueTpl[d.slot]));
            valueTemplate = gDiscoveryHeap->analogValueTpl[d.slot];

        }
        const HASensorEntry entry{"io", d.haSuffix, name, d.topic, valueTemplate, nullptr,
            d.kind == DataValueKind::Tank ? "mdi:cup-water" :
                (d.kind == DataValueKind::Uptime ? "mdi:timer-outline" : "mdi:chart-line"), unit[0] ? unit : nullptr,
            true, analog ? "{{ 'online' if value_json.available else 'offline' }}" :
                           "{{ 'online' if value_json.quality == 1 else 'offline' }}", false, nullptr, d.haSuffix, d.id};
        reportDiscoveryResult(modules.haService->addSensor(modules.haService->ctx, &entry), d.haSuffix);
    }
}

void syncSwitches(const DomainSpec& domain, ModuleInstances& modules)
{
    if (!modules.haService || !modules.haService->addSwitch) return;
    if (!reportDiscoveryResult(ensureDiscoveryHeap(), "discovery heap")) return;

    for (uint8_t i = 0; i < domain.poolDeviceCount; ++i) {
        const PoolDevicePreset& device = domain.poolDevices[i];
        const DomainSlotPreset* commandSlot = findDomainSlotById(domain, device.commandSlot);
        if (!commandSlot) continue;
        if (!reportDiscoveryResult(device.id < PoolIds::DeviceCount, "pool role index")) continue;
        const auto& role = gDiscoveryHeap->poolRoles.roles[device.id];

        const HASwitchEntry entry{
            "io",
            device.objectSuffix,
            commandSlot->displayName,
            role.state,
            "{% if value_json.on %}ON{% else %}OFF{% endif %}",
            MqttTopics::SuffixCmd,
            role.on,
            role.off,
            device.haIcon,
            nullptr
        };
        reportDiscoveryResult(modules.haService->addSwitch(modules.haService->ctx, &entry), entry.objectSuffix);
    }
}

}  // namespace

namespace Profiles {
namespace Waveshare {

void configureIoModule(const AppContext& ctx, ModuleInstances& modules)
{
    requireSetup(ctx.domain != nullptr, "missing domain spec");

    modules.ioModule.setOneWireBuses(&modules.oneWireWater, &modules.oneWireAir);
    modules.ioModule.setBindingPorts(
        FlowIoLayout::kBindingPorts,
        (uint8_t)(sizeof(FlowIoLayout::kBindingPorts) / sizeof(FlowIoLayout::kBindingPorts[0]))
    );
    modules.ioModule.setExpanders(
        FlowIoLayout::kExpanders,
        (uint8_t)(sizeof(FlowIoLayout::kExpanders) / sizeof(FlowIoLayout::kExpanders[0]))
    );

    for (uint8_t i = 0; i < Limits::Io::MaxAnalogEndpoints; ++i) {
        IOAnalogDefinition def{};
        snprintf(def.id, sizeof(def.id), "a%02u", (unsigned)i);
        def.ioId = (IoId)(IO_ID_AI_BASE + i);
        applyAnalogDefaultsForIoSlot(analogInputSlot(i), def);
        requireSetup(modules.ioModule.defineAnalogInput(def), "define analog input slot");
    }

    for (uint8_t i = 0; i < ctx.domain->domainSlotCount; ++i) {
        const DomainSlotPreset& preset = ctx.domain->domainSlots[i];
        const IoSlotId ioSlot = findIoSlotForDomainSlot(*ctx.domain, preset.id);
        if (ioSlot == IO_SLOT_INVALID) continue;
        const IoId ioId = ioIdFromSlot(ioSlot);
        requireSetup(ioId != IO_ID_INVALID, "invalid domain slot IO mapping");

        if (preset.slotKind == IO_SLOT_DIGITAL_INPUT) {
            IODigitalInputDefinition def{};
            snprintf(def.id, sizeof(def.id), "%s", preset.endpointId ? preset.endpointId : "input");
            def.ioId = ioId;
            def.pullMode = IO_PULL_NONE;
            applyDigitalDefaultsForDomainSlot(preset.id, def);
            if (const char* defaultName = waveshareDigitalInputNameForDomainSlot(preset.id)) {
                snprintf(def.id, sizeof(def.id), "%s", defaultName);
            }
            requireSetup(modules.ioModule.defineDigitalInput(def), "define digital input");
            continue;
        }
    }

    for (uint8_t i = 0; i < 8U && i < Limits::Io::MaxDigitalInputs; ++i) {
        if (modules.ioModule.digitalInputSlotUsed(i)) continue;
        IODigitalInputDefinition def{};
        snprintf(def.id, sizeof(def.id), "%s", waveshareDigitalInputNameForLogical(i));
        def.ioId = (IoId)(IO_ID_DI_BASE + i);
        def.activeHigh = false;
        def.pullMode = IO_PULL_UP;
        def.mode = IO_DIGITAL_INPUT_STATE;
        def.edgeMode = IO_EDGE_RISING;
        def.counterDebounceUs = 0U;
        const PhysicalPortId defaultPort = digitalInputPortFromOrdinal((uint8_t)(i + 1U));
        def.binding.primaryPort = FlowIoLayout::digitalInputPortUsedByDomainRole(defaultPort)
            ? IO_PORT_INVALID
            : defaultPort;
        requireSetup(modules.ioModule.defineDigitalInput(def), "define waveshare digital input");
    }
    for (uint8_t logicalIdx = 8U; logicalIdx < Limits::Io::MaxDigitalInputs; ++logicalIdx) {
        const uint8_t i = (uint8_t)(logicalIdx - 8U);
        if (modules.ioModule.digitalInputSlotUsed(logicalIdx)) continue;
        IODigitalInputDefinition def{};
        snprintf(def.id, sizeof(def.id), "%s", waveshareDigitalInputNameForLogical(logicalIdx));
        def.ioId = (IoId)(IO_ID_DI_BASE + logicalIdx);
        def.activeHigh = false;
        def.pullMode = IO_PULL_NONE;
        def.mode = IO_DIGITAL_INPUT_STATE;
        def.edgeMode = IO_EDGE_RISING;
        def.counterDebounceUs = 0U;
        def.binding.primaryPort = waveshareMcpInputPort(i);
        requireSetup(modules.ioModule.defineDigitalInput(def), "define waveshare MCP digital input");
    }

    for (uint8_t i = 0; i < ctx.domain->domainSlotCount; ++i) {
        const DomainSlotPreset& preset = ctx.domain->domainSlots[i];
        if (preset.slotKind != IO_SLOT_DIGITAL_OUTPUT) continue;
        const IoSlotId ioSlot = findIoSlotForDomainSlot(*ctx.domain, preset.id);
        if (ioSlot == IO_SLOT_INVALID) continue;
        requireSetup(ioSlotKind(ioSlot) == IO_SLOT_DIGITAL_OUTPUT, "domain output mapped to non-output slot");

        const FlowIoLayout::DigitalOutputRoleDefault* spec = FlowIoLayout::digitalOutputDefaultForDomainSlot(preset.id);
        requireSetup(spec != nullptr, "missing output layout binding");

        IODigitalOutputDefinition def{};
        snprintf(def.id, sizeof(def.id), "%s", preset.displayName ? preset.displayName : "output");
        def.ioId = ioIdFromSlot(ioSlot);
        def.binding.primaryPort = spec->bindingPort;
        def.activeHigh = spec->activeHigh;
        def.initialOn = false;
        def.retainOnWarmReboot = spec->retainOnWarmReboot;
        def.momentary = spec->momentary;
        def.pulseMs = spec->momentary ? spec->pulseMs : 0;
        requireSetup(modules.ioModule.defineDigitalOutput(def), "define digital output");
    }

    if (!modules.ioModule.digitalOutputSlotUsed(6U)) {
        IODigitalOutputDefinition def{};
        snprintf(def.id, sizeof(def.id), "Lights");
        def.ioId = (IoId)(IO_ID_DO_BASE + 6U);
        def.binding.primaryPort = FlowIoLayout::PortExio7;
        def.activeHigh = true;
        def.initialOn = false;
        def.retainOnWarmReboot = false;
        def.momentary = false;
        def.pulseMs = 0;
        requireSetup(modules.ioModule.defineDigitalOutput(def), "define lights EXIO output");
    }

    static constexpr uint8_t kMcpOutputCount = 8U;
    for (uint8_t logicalIdx = 8U;
         logicalIdx < Limits::Io::MaxDigitalOutputs && logicalIdx < (uint8_t)(8U + kMcpOutputCount);
         ++logicalIdx) {
        const uint8_t i = (uint8_t)(logicalIdx - 8U);
        IODigitalOutputDefinition def{};
        snprintf(def.id, sizeof(def.id), "MCP B%u", (unsigned)i);
        def.ioId = (IoId)(IO_ID_DO_BASE + logicalIdx);
        def.binding.primaryPort = waveshareCompOutputPort(i);
        def.activeHigh = true;
        def.initialOn = false;
        def.retainOnWarmReboot = false;
        def.momentary = false;
        def.pulseMs = 0;
        requireSetup(modules.ioModule.defineDigitalOutput(def), "define comp digital output");
    }
}

void registerIoHomeAssistant(AppContext& ctx, ModuleInstances& modules)
{
    modules.haService = ctx.services.get<HAService>(ServiceId::Ha);
    if (!modules.haService) return;

    // All onConfigLoaded callbacks have completed; capture effective assignments once.
    const auto* poolConfig = ctx.services.get<PoolConfigurationService>(ServiceId::PoolConfiguration);
    PoolDeviceAssignments assignments{};
    const bool assignmentsReady = reportDiscoveryResult(
        poolConfig && poolConfig->getDeviceAssignments &&
        poolConfig->getDeviceAssignments(poolConfig->ctx, &assignments),
        "PoolLogic device assignments");
    if (!reportDiscoveryResult(ensureDiscoveryHeap(), "discovery heap")) return;
    const bool rolesReady = assignmentsReady && reportDiscoveryResult(
        PoolRoleHaDiscovery::prepare(gDiscoveryHeap->poolRoles, assignments,
                                     Limits::Io::MaxPoolDevices), "pool role topics");
    if (rolesReady) {
        reportDiscoveryResult(
            PoolRoleHaDiscovery::registerEntries(*modules.haService, gDiscoveryHeap->poolRoles),
            "pool role entities");
    }

    syncDataSensors(modules);
    if (rolesReady && ctx.domain) syncSwitches(*ctx.domain, modules);

    if (modules.haService->requestRefresh) {
        (void)modules.haService->requestRefresh(modules.haService->ctx);
    }
}

void refreshIoHomeAssistantIfNeeded(ModuleInstances& modules)
{
#if FLOW_HA_ONESHOT_DISCOVERY
    if (!gOneShotRefreshBypassedLogged) {
        WAVESHARE_HA_BOOT_TRACE("flow.io IO->HA dynamic refresh bypassed in one-shot mode");
        gOneShotRefreshBypassedLogged = true;
    }
    releaseDiscoveryHeapIfReady(modules);
    return;
#endif
    if (!modules.haService) return;
    const uint32_t dirtyMask = modules.ioModule.takeAnalogConfigDirtyMask();
    if (dirtyMask == 0) return;

    syncDataSensors(modules);
    if (modules.haService->requestRefresh) {
        (void)modules.haService->requestRefresh(modules.haService->ctx);
    }
}

void releaseIoHomeAssistantDiscoveryHeapIfDone(ModuleInstances& modules)
{
    releaseDiscoveryHeapIfReady(modules);
}

}  // namespace Waveshare
}  // namespace Profiles

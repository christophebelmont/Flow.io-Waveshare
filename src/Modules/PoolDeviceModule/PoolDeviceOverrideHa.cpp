#include "PoolDeviceModule.h"
#include "PoolDeviceOverrideDiscovery.h"
#include "Core/SpiRamJsonDocument.h"
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::PoolDeviceModule)
#include "Core/ModuleLog.h"

void PoolDeviceModule::registerOverrideHa_()
{
    using namespace PoolDeviceOverrideDiscovery;
    SpiRamJsonDocument options(2048);
    auto array = options.to<JsonArray>();
    for (uint8_t i = 0; i < POOL_DEVICE_MAX; ++i) {
        if (!slots_[i].def.enabled || !overrideSupported_(i)) continue;
        auto& slot = slots_[i];
        snprintf(slot.overrideOption, sizeof(slot.overrideOption), "%s — %s", slot.id, slot.def.label);
        array.add(slot.overrideOption);
        if (overrideSelection_ == 0xFF) overrideSelection_ = i;
    }
    if (options.overflowed() || measureJson(options) >= sizeof(overrideOptions_)) {
        LOGE("Override selector catalogue exceeds capacity"); return;
    }
    serializeJson(options, overrideOptions_, sizeof(overrideOptions_));
    if (!haSvc_ || array.size() == 0) return;
    bool ok = true;
    if (!haSvc_->addNumber || !haSvc_->addNumber(haSvc_->ctx, &Duration)) ok = false;
    for (const auto& sensor : Sensors) if (!haSvc_->addSensor || !haSvc_->addSensor(haSvc_->ctx, &sensor)) ok = false;
    for (const auto& button : Buttons) if (!haSvc_->addButton || !haSvc_->addButton(haSvc_->ctx, &button)) ok = false;
    const HASelectEntry selector = {"pooldevice", "pdm_ovr_target", "Override Actuator", Selected,
        "{{ value_json.selection }}", "cmd",
        "{{ {'cmd': 'pooldevice.override.select', 'args': {'option': value}} | to_json }}",
        overrideOptions_, "mdi:format-list-bulleted", nullptr};
    if (!haSvc_->addSelect || !haSvc_->addSelect(haSvc_->ctx, &selector)) ok = false;
    if (!ok) LOGE("Consolidated override discovery registration incomplete");
}

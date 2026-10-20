#include "IOModule.h"
#include "Core/I2cAddressSelection.h"
#include "Core/I2cStartupReadiness.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

namespace {
IOPortPresence presenceForAddress(const I2cAddressSelection& device)
{
    switch (device.status) {
        case I2cAddressStatus::Primary:
        case I2cAddressStatus::Secondary: return IOPortPresence::Present;
        case I2cAddressStatus::Missing: return IOPortPresence::Absent;
        case I2cAddressStatus::Disabled: return IOPortPresence::Disabled;
        case I2cAddressStatus::DetectionFailed: return IOPortPresence::DetectionFailed;
        default: return IOPortPresence::Invalid;
    }
}

} // namespace

void IOModule::awaitI2cStartupReadiness_()
{
    if (!i2cBus_ || !i2cBus_->beginOk()) return;
    I2cStartupDevice devices[IO_SRC_COUNT + IO_MAX_EXPANDERS]{};
    for (uint8_t i = 0; i < IO_SRC_COUNT; ++i)
        devices[i].address = analogI2cAddresses_[i];
    for (uint8_t i = 0; i < IO_MAX_EXPANDERS; ++i)
        devices[IO_SRC_COUNT + i].address = expanderI2cAddresses_[i];

    // Discovery can precede other hardware initialization. An ACK at discovery
    // is not a readiness guarantee when providers are finally initialized.
    constexpr uint32_t startupBudgetMs = 250;
    awaitI2cStartupDevices(devices, IO_SRC_COUNT + IO_MAX_EXPANDERS, startupBudgetMs,
        [this](uint8_t address, uint32_t remainingMs) {
            const uint32_t startedProbe = millis();
            if (!i2cBus_->lock(remainingMs)) return I2cProbeResult::Error;
            const uint32_t elapsed = millis() - startedProbe;
            I2cProbeResult result = I2cProbeResult::Error;
            if (elapsed < remainingMs) {
                TwoWire* wire = i2cBus_->wire();
                const uint16_t previousTimeout = wire->getTimeOut();
                const uint32_t available = remainingMs - elapsed;
                wire->setTimeOut(static_cast<uint16_t>(
                    available < previousTimeout ? available : previousTimeout));
                result = i2cBus_->probePresence(address);
                wire->setTimeOut(previousTimeout);
            }
            i2cBus_->unlock();
            return result;
        },
        []() { return static_cast<uint32_t>(millis()); },
        [](uint32_t remainingMs) { delay(remainingMs < 2U ? remainingMs : 2U); });

    for (size_t i = 0; i < IO_SRC_COUNT + IO_MAX_EXPANDERS; ++i) {
        const auto& device = devices[i];
        if (!device.address || device.ready) continue;
        if (i < IO_SRC_COUNT) {
            analogI2cAddresses_[i] = 0;
            analogPresence_[i] = IOPortPresence::DetectionFailed;
        } else {
            expanderI2cAddresses_[i - IO_SRC_COUNT] = 0;
            expanderPresence_[i - IO_SRC_COUNT] = IOPortPresence::DetectionFailed;
        }
    }
}

void IOModule::resolveI2cAddresses_(const bool* needAnalogSource)
{
    I2cAddressSelection devices[IO_SRC_COUNT + IO_MAX_EXPANDERS]{};
    const char* names[IO_SRC_COUNT]{};
    const auto addAnalog = [&](uint8_t source, const char* name, bool enabled,
                               uint8_t primary, uint8_t secondary) {
        devices[source] = {primary, secondary, enabled};
        names[source] = name;
    };
    // ADS providers have no enable switch: a configured binding enables them.
    addAnalog(IO_SRC_ADS_INTERNAL_SINGLE, "ads1115_int", needAnalogSource[IO_SRC_ADS_INTERNAL_SINGLE],
              cfgData_.adsInternalAddr, cfgData_.adsInternalSecondaryAddr);
    addAnalog(IO_SRC_ADS_EXTERNAL_DIFF, "ads1115_ext", needAnalogSource[IO_SRC_ADS_EXTERNAL_DIFF],
              cfgData_.adsExternalAddr, cfgData_.adsExternalSecondaryAddr);
    addAnalog(IO_SRC_SHT40, "sht40", cfgData_.sht40Enabled,
              cfgData_.sht40Address, cfgData_.sht40SecondaryAddress);
    addAnalog(IO_SRC_BMP280, "bmp280", cfgData_.bmp280Enabled,
              cfgData_.bmp280Address, cfgData_.bmp280SecondaryAddress);
    addAnalog(IO_SRC_BME680, "bme680", cfgData_.bme680Enabled,
              cfgData_.bme680Address, cfgData_.bme680SecondaryAddress);
    addAnalog(IO_SRC_INA226, "ina226", cfgData_.ina226Enabled,
              cfgData_.ina226Address, cfgData_.ina226SecondaryAddress);
    for (uint8_t i = 0; i < IO_MAX_EXPANDERS; ++i) {
        devices[IO_SRC_COUNT + i] = {expanderCfg_[i].address,
                                    expanderCfg_[i].secondaryAddress,
                                    expanderUsable_(i)};
        expanderI2cAddresses_[i] = 0;
    }
    for (auto& address : analogI2cAddresses_) address = 0;
    if (!i2cBus_ || !i2cBus_->beginOk() || !i2cBus_->lock(50)) {
        for (auto& state : analogPresence_) state = IOPortPresence::DetectionFailed;
        for (auto& state : expanderPresence_) state = IOPortPresence::DetectionFailed;
        LOGW("I2C address selection: bus unavailable");
        return;
    }
    resolveI2cAddresses(devices, IO_SRC_COUNT + IO_MAX_EXPANDERS,
                        [this](uint8_t address) { return i2cBus_->probePresence(address); });
    i2cBus_->unlock();

    for (uint8_t i = 0; i < IO_SRC_COUNT; ++i) {
        analogI2cAddresses_[i] = devices[i].active;
        analogPresence_[i] = presenceForAddress(devices[i]);
        if (!devices[i].enabled) continue;
        LOGI("I2C %s primary=0x%02X secondary=0x%02X active=0x%02X: %s",
             names[i], devices[i].primary, devices[i].secondary, devices[i].active,
             i2cAddressStatusName(devices[i].status));
    }
    for (uint8_t i = 0; i < IO_MAX_EXPANDERS; ++i) {
        const auto& device = devices[IO_SRC_COUNT + i];
        expanderI2cAddresses_[i] = device.active;
        expanderPresence_[i] = presenceForAddress(device);
        if (!device.enabled) continue;
        LOGI("I2C expander=%u primary=0x%02X secondary=0x%02X active=0x%02X: %s",
             (unsigned)i, device.primary, device.secondary, device.active,
             i2cAddressStatusName(device.status));
    }
}

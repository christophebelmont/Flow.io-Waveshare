/**
 * @file Ds18b20Driver.cpp
 * @brief Implementation file.
 */

#include "Ds18b20Driver.h"
#include "Modules/IOModule/IOBus/OneWireBus.h"
#include <Arduino.h>
#include <DallasTemperature.h>
#include <string.h>

Ds18b20Driver::Ds18b20Driver(const char* driverId, OneWireBus* bus, const uint8_t address[8],
                             const Ds18b20DriverConfig& cfg)
    : driverId_(driverId), bus_(bus), pollMs_(cfg.periodMs())
{
    if (address) memcpy(address_, address, sizeof(address_));
}

bool Ds18b20Driver::begin()
{
    if (!bus_) return false;
    bus_->begin();
    scheduleFirstConversion(millis(), 0U);
    valid_ = false;
    return true;
}

void Ds18b20Driver::scheduleFirstConversion(uint32_t nowMs, uint32_t delayMs)
{
    requestAnchorMs_ = nowMs;
    requestDelayMs_ = delayMs;
    conversionPending_ = false;
}

Ds18b20Driver::PollStep Ds18b20Driver::poll(uint32_t nowMs)
{
    if (!bus_) return PollStep::Idle;
    if (conversionPending_) {
        if (nowMs - conversionStartedMs_ < Ds18b20DriverConfig::kConversionWaitMs) return PollStep::Idle;
        const float c = bus_->readC(address_);
        valid_ = c != DEVICE_DISCONNECTED_C;
        if (valid_) celsius_ = c;
        conversionPending_ = false;
        return PollStep::Read;
    }

    const uint32_t elapsedMs = nowMs - requestAnchorMs_;
    if (elapsedMs < requestDelayMs_) return PollStep::Idle;
    // Keep the initial phase even when IO misses an acquisition deadline.
    const uint32_t latenessMs = elapsedMs - requestDelayMs_;
    requestAnchorMs_ = nowMs - (latenessMs % pollMs_);
    requestDelayMs_ = pollMs_;
    bus_->request();
    conversionStartedMs_ = millis();
    conversionPending_ = true;
    return PollStep::Convert;
}

bool Ds18b20Driver::readCelsius(float& out) const
{
    if (!valid_) return false;
    out = celsius_;
    return true;
}

bool Ds18b20Driver::readSample(uint8_t, IOAnalogSample& out) const
{
    float c = 0.0f;
    if (!readCelsius(c)) return false;
    out = IOAnalogSample{};
    out.value = c;
    return true;
}

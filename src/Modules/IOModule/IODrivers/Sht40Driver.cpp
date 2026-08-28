/**
 * @file Sht40Driver.cpp
 * @brief Implementation file.
 */

#include "Sht40Driver.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

Sht40Driver::Sht40Driver(const char* driverId, I2CBus* bus, const Sht40DriverConfig& cfg)
    : driverId_(driverId), bus_(bus), cfg_(cfg)
{
}

bool Sht40Driver::begin()
{
    if (!bus_) return false;
    if (cfg_.address != SHT4x_DEFAULT_ADDR) {
        LOGW("SHT40 %s unsupported address 0x%02X (fixed 0x%02X)",
             driverId_ ? driverId_ : "sensor",
             cfg_.address,
             SHT4x_DEFAULT_ADDR);
        return false;
    }
    if (!bus_->lock(50)) return false;

    const bool ok = sht_.begin(bus_->wire());
    if (ok) {
        sht_.setPrecision(SHT4X_HIGH_PRECISION);
        sht_.setHeater(SHT4X_NO_HEATER);
        ready_ = true;
        valid_ = false;
        lastPollMs_ = 0;
        seq_ = 0;
    }

    bus_->unlock();

    if (!ok) {
        LOGW("SHT40 %s not detected at 0x%02X", driverId_ ? driverId_ : "sensor", cfg_.address);
    }
    return ok;
}

void Sht40Driver::tick(uint32_t nowMs)
{
    if (!ready_ || !bus_) return;
    if ((uint32_t)(nowMs - lastPollMs_) < cfg_.pollMs) return;
    lastPollMs_ = nowMs;

    if (!bus_->lock(20)) return;

    sensors_event_t humidity{};
    sensors_event_t temperature{};
    const bool ok = sht_.getEvent(&humidity, &temperature);

    bus_->unlock();

    if (!ok) return;

    values_[0] = temperature.temperature;
    values_[1] = humidity.relative_humidity;
    valid_ = true;
    ++seq_;
}

bool Sht40Driver::readSample(uint8_t channel, IOAnalogSample& out) const
{
    out = IOAnalogSample{};
    if (!valid_ || channel > 1) return false;

    out.value = values_[channel];
    out.seq = seq_;
    out.hasSeq = true;
    return true;
}

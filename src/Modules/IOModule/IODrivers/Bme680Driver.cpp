/**
 * @file Bme680Driver.cpp
 * @brief Implementation file.
 */

#include "Bme680Driver.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

Bme680Driver::Bme680Driver(const char* driverId, I2CBus* bus, const Bme680DriverConfig& cfg)
    : driverId_(driverId), bus_(bus), cfg_(cfg), bme_(bus ? bus->wire() : &Wire)
{
}

bool Bme680Driver::begin()
{
    if (!bus_) return false;
    if (!bus_->lock(50)) return false;

    const bool ok = bme_.begin(cfg_.address);
    if (ok) {
        bme_.setIIRFilterSize(BME680_FILTER_SIZE_3);
        bme_.setHumidityOversampling(BME680_OS_2X);
        bme_.setPressureOversampling(BME680_OS_4X);
        bme_.setTemperatureOversampling(BME680_OS_8X);
        bme_.setGasHeater(320, 150);
        ready_ = true;
        valid_ = false;
        reading_ = false;
        lastPollMs_ = 0;
        readReadyAtMs_ = 0;
        seq_ = 0;
    }

    bus_->unlock();

    if (!ok) {
        LOGW("BME680 %s not detected at 0x%02X", driverId_ ? driverId_ : "sensor", cfg_.address);
    }
    return ok;
}

void Bme680Driver::tick(uint32_t nowMs)
{
    if (!ready_ || !bus_) return;

    if (reading_) {
        if ((int32_t)(nowMs - readReadyAtMs_) < 0) return;
        if (!bus_->lock(20)) return;
        const bool ok = bme_.endReading();
        bus_->unlock();
        reading_ = false;
        if (!ok) return;

        values_[0] = bme_.temperature;
        values_[1] = bme_.humidity;
        values_[2] = bme_.pressure / 100.0f;
        values_[3] = (float)bme_.gas_resistance;
        valid_ = true;
        ++seq_;
        return;
    }

    if ((uint32_t)(nowMs - lastPollMs_) < cfg_.pollMs) return;
    if (!bus_->lock(20)) return;

    const uint32_t readyAtMs = bme_.beginReading();

    bus_->unlock();

    lastPollMs_ = nowMs;
    if (readyAtMs == 0U) return;

    reading_ = true;
    readReadyAtMs_ = readyAtMs;
}

bool Bme680Driver::readSample(uint8_t channel, IOAnalogSample& out) const
{
    out = IOAnalogSample{};
    if (!valid_ || channel > 3) return false;

    out.value = values_[channel];
    out.seq = seq_;
    out.hasSeq = true;
    return true;
}

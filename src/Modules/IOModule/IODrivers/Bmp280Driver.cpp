/**
 * @file Bmp280Driver.cpp
 * @brief Implementation file.
 */

#include "Bmp280Driver.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

#include <math.h>

Bmp280Driver::Bmp280Driver(const char* driverId, I2CBus* bus, const Bmp280DriverConfig& cfg)
    : driverId_(driverId), bus_(bus), cfg_(cfg), bmp_(bus ? bus->wire() : &Wire)
{
}

bool Bmp280Driver::begin()
{
    if (!bus_) return false;
    if (!bus_->lock(50)) return false;

    const bool ok = bmp_.begin(cfg_.address);
    if (ok) {
        bmp_.setSampling(Adafruit_BMP280::MODE_NORMAL,
                         Adafruit_BMP280::SAMPLING_X2,
                         Adafruit_BMP280::SAMPLING_X16,
                         Adafruit_BMP280::FILTER_X4,
                         Adafruit_BMP280::STANDBY_MS_250);
        ready_ = true;
        valid_ = false;
        lastPollMs_ = 0;
        seq_ = 0;
    }

    bus_->unlock();

    if (!ok) {
        LOGW("BMP280 %s not detected at 0x%02X", driverId_ ? driverId_ : "sensor", cfg_.address);
    }
    return ok;
}

void Bmp280Driver::tick(uint32_t nowMs)
{
    if (!ready_ || !bus_) return;
    if ((uint32_t)(nowMs - lastPollMs_) < cfg_.pollMs) return;
    lastPollMs_ = nowMs;

    if (!bus_->lock(20)) return;

    const float temperatureC = bmp_.readTemperature();
    const float pressureHpa = bmp_.readPressure() / 100.0f;

    bus_->unlock();

    if (!isfinite(temperatureC) || !isfinite(pressureHpa)) return;

    values_[0] = temperatureC;
    values_[1] = pressureHpa;
    valid_ = true;
    ++seq_;
}

bool Bmp280Driver::readSample(uint8_t channel, IOAnalogSample& out) const
{
    out = IOAnalogSample{};
    if (!valid_ || channel > 1) return false;

    out.value = values_[channel];
    out.seq = seq_;
    out.hasSeq = true;
    return true;
}

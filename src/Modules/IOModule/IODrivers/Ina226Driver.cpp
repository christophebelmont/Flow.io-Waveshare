/**
 * @file Ina226Driver.cpp
 * @brief Implementation file.
 */

#include "Ina226Driver.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

namespace {
constexpr float kIna226FullScaleShuntVoltage = 0.0819f;
}

Ina226Driver::Ina226Driver(const char* driverId, I2CBus* bus, const Ina226DriverConfig& cfg)
    : driverId_(driverId), bus_(bus), cfg_(cfg), ina_(bus ? bus->wire() : &Wire, cfg.address)
{
}

bool Ina226Driver::begin()
{
    if (!bus_) return false;
    if (cfg_.shuntOhms <= 0.0f) {
        LOGW("INA226 %s invalid shunt %.6f Ohm", driverId_ ? driverId_ : "sensor", (double)cfg_.shuntOhms);
        return false;
    }
    if (!bus_->lock(50)) return false;

    const bool ok = ina_.init();
    if (ok) {
        const float maxCurrentA = kIna226FullScaleShuntVoltage / cfg_.shuntOhms;
        ina_.setAverage(INA226_AVERAGE_16);
        ina_.setConversionTime(INA226_CONV_TIME_1100, INA226_CONV_TIME_1100);
        ina_.setMeasureMode(INA226_CONTINUOUS);
        ina_.setResistorRange(cfg_.shuntOhms, maxCurrentA);
        ina_.waitUntilConversionCompleted();
    }
    if (ok) {
        ready_ = true;
        valid_ = false;
        lastPollMs_ = 0;
        seq_ = 0;
    }

    bus_->unlock();

    if (!ok) {
        LOGW("INA226 %s not ready at 0x%02X", driverId_ ? driverId_ : "sensor", cfg_.address);
    }
    return ok;
}

void Ina226Driver::tick(uint32_t nowMs)
{
    if (!ready_ || !bus_) return;
    if ((uint32_t)(nowMs - lastPollMs_) < cfg_.pollMs) return;
    lastPollMs_ = nowMs;

    if (!bus_->lock(20)) return;

    const float shuntMv = ina_.getShuntVoltage_mV();
    if (ina_.getI2cErrorCode() != 0) {
        bus_->unlock();
        return;
    }

    const float busV = ina_.getBusVoltage_V();
    if (ina_.getI2cErrorCode() != 0) {
        bus_->unlock();
        return;
    }

    const float currentMa = ina_.getCurrent_mA();
    if (ina_.getI2cErrorCode() != 0) {
        bus_->unlock();
        return;
    }

    const float powerMw = ina_.getBusPower();
    if (ina_.getI2cErrorCode() != 0) {
        bus_->unlock();
        return;
    }

    bus_->unlock();

    values_[0] = shuntMv;
    values_[1] = busV;
    values_[2] = currentMa;
    values_[3] = powerMw;
    values_[4] = busV + (shuntMv / 1000.0f);
    valid_ = true;
    ++seq_;
}

bool Ina226Driver::readSample(uint8_t channel, IOAnalogSample& out) const
{
    out = IOAnalogSample{};
    if (!valid_ || channel > 4) return false;

    out.value = values_[channel];
    out.seq = seq_;
    out.hasSeq = true;
    return true;
}

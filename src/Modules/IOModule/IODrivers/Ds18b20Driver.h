#pragma once
/**
 * @file Ds18b20Driver.h
 * @brief DS18B20 async request/read driver.
 */

#include <stdint.h>
#include "Core/ConfigDefaults.h"
#include "Modules/IOModule/IODrivers/IODriver.h"

class OneWireBus;

struct Ds18b20DriverConfig {
    static constexpr uint32_t kConversionWaitMs = 750U;
    uint32_t pollMs = FLOW_MODDEF_IO_DS;

    uint32_t periodMs() const { return pollMs < kConversionWaitMs ? kConversionWaitMs : pollMs; }
};

class Ds18b20Driver : public IAnalogSourceDriver {
public:
    enum class PollStep : uint8_t { Idle, Convert, Read };

    Ds18b20Driver(const char* driverId, OneWireBus* bus, const uint8_t address[8],
                  const Ds18b20DriverConfig& cfg);

    const char* id() const override { return driverId_; }
    bool begin() override;
    void tick(uint32_t nowMs) override { (void)poll(nowMs); }
    void scheduleFirstConversion(uint32_t nowMs, uint32_t delayMs);
    PollStep poll(uint32_t nowMs);

    bool readCelsius(float& out) const;
    bool readSample(uint8_t channel, IOAnalogSample& out) const override;

private:
    const char* driverId_ = nullptr;
    OneWireBus* bus_ = nullptr;
    uint8_t address_[8] = {0};
    uint32_t pollMs_;

    uint32_t requestAnchorMs_ = 0;
    uint32_t requestDelayMs_ = 0;
    uint32_t conversionStartedMs_ = 0;
    bool conversionPending_ = false;
    bool valid_ = false;
    float celsius_ = 0.0f;
};

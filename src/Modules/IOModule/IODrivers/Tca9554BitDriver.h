#pragma once
/**
 * @file Tca9554BitDriver.h
 * @brief Logical single-bit digital output adapter on top of Tca9554Driver.
 */

#include "Modules/IOModule/IODrivers/Tca9554Driver.h"

class Tca9554BitDriver : public IDigitalPinDriver {
public:
    Tca9554BitDriver(const char* driverId, Tca9554Driver* parent, uint8_t bit, bool activeHigh);

    const char* id() const override { return driverId_; }
    bool begin() override;
    void tick(uint32_t) override {}

    bool write(bool on) override;
    bool read(bool& on) const override;

private:
    const char* driverId_ = nullptr;
    Tca9554Driver* parent_ = nullptr;
    uint8_t bit_ = 0;
    bool activeHigh_ = true;
};

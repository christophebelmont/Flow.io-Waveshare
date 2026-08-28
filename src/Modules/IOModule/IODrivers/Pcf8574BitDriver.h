#pragma once
/**
 * @file Pcf8574BitDriver.h
 * @brief Logical single-bit digital output adapter on top of Pcf8574Driver.
 */

#include "Modules/IOModule/IODrivers/Pcf8574Driver.h"

class Pcf8574BitDriver : public IDigitalPinDriver {
public:
    Pcf8574BitDriver(const char* driverId, Pcf8574Driver* parent, uint8_t bit, bool activeHigh);

    const char* id() const override { return driverId_; }
    bool begin() override;
    void tick(uint32_t) override {}

    bool write(bool on) override;
    bool read(bool& on) const override;

private:
    const char* driverId_ = nullptr;
    Pcf8574Driver* parent_ = nullptr;
    uint8_t bit_ = 0;
    bool activeHigh_ = true;
};

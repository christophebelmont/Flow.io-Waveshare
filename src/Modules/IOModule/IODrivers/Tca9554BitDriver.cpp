/**
 * @file Tca9554BitDriver.cpp
 * @brief Implementation file.
 */

#include "Tca9554BitDriver.h"

Tca9554BitDriver::Tca9554BitDriver(const char* driverId, Tca9554Driver* parent, uint8_t bit, bool activeHigh)
    : driverId_(driverId), parent_(parent), bit_(bit), activeHigh_(activeHigh)
{
}

bool Tca9554BitDriver::begin()
{
    return parent_ != nullptr;
}

bool Tca9554BitDriver::write(bool on)
{
    if (!parent_) return false;
    const bool rawOn = activeHigh_ ? on : !on;
    return parent_->writePin(bit_, rawOn);
}

bool Tca9554BitDriver::read(bool& on) const
{
    if (!parent_) return false;
    bool rawOn = false;
    if (!parent_->readShadow(bit_, rawOn)) return false;
    on = activeHigh_ ? rawOn : !rawOn;
    return true;
}

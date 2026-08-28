/**
 * @file Pcf8574BitDriver.cpp
 * @brief Implementation file.
 */

#include "Pcf8574BitDriver.h"

Pcf8574BitDriver::Pcf8574BitDriver(const char* driverId, Pcf8574Driver* parent, uint8_t bit, bool activeHigh)
    : driverId_(driverId), parent_(parent), bit_(bit), activeHigh_(activeHigh)
{
}

bool Pcf8574BitDriver::begin()
{
    return parent_ != nullptr;
}

bool Pcf8574BitDriver::write(bool on)
{
    if (!parent_) return false;
    const bool rawOn = activeHigh_ ? on : !on;
    return parent_->writePin(bit_, rawOn);
}

bool Pcf8574BitDriver::read(bool& on) const
{
    if (!parent_) return false;
    bool rawOn = false;
    if (!parent_->readShadow(bit_, rawOn)) return false;
    on = activeHigh_ ? rawOn : !rawOn;
    return true;
}

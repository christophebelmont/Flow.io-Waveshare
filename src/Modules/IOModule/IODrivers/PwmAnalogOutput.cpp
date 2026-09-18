#include "PwmAnalogOutput.h"
#include <Arduino.h>
#include <cmath>

bool PwmAnalogOutput::begin(uint8_t pin, uint32_t frequency, uint8_t resolution, float maximum, float safeValue)
{
    if (ready_ || !std::isfinite(maximum) || maximum <= 0 || !std::isfinite(safeValue) ||
        safeValue < 0 || safeValue > maximum || !frequency || resolution < 1 || resolution > 14) return false;
    if (!ledcAttach(pin, frequency, resolution)) return false;
    pin_ = pin; resolution_ = resolution; maximum_ = maximum; ready_ = true;
    if (write(safeValue)) return true;
    ledcDetach(pin_); ready_ = false; return false;
}
bool PwmAnalogOutput::write(float value)
{
    if (!ready_ || !std::isfinite(value) || value < 0 || value > maximum_) return false;
    const uint32_t duty = uint32_t(std::lround(value / maximum_ * ((1U << resolution_) - 1U)));
    return ledcWrite(pin_, duty);
}

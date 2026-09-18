#pragma once
#include <stdint.h>

/** Board-owned PWM converter. External filtering/level conversion defines the electrical range.
 * The profile must reserve a free physical GPIO before begin(); relay pins are not PWM pins.
 */
class PwmAnalogOutput {
public:
    bool begin(uint8_t gpio, uint32_t frequencyHz, uint8_t resolutionBits, float maximum, float safeValue);
    bool write(float value);
    static bool writeValue(void* context, float value) {
        return context && static_cast<PwmAnalogOutput*>(context)->write(value);
    }
private:
    uint8_t pin_ = 0;
    uint8_t resolution_ = 0;
    float maximum_ = 0;
    bool ready_ = false;
};

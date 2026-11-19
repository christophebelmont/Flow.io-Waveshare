#pragma once
#include <stdint.h>

class OneWire {
public:
    void begin(uint8_t pin) { pin_ = pin; }
    uint8_t pin() const { return pin_; }

private:
    uint8_t pin_ = 0;
};

#pragma once
#include "OneWire.h"

constexpr float DEVICE_DISCONNECTED_C = -127.0f;

class DallasTemperature {
public:
    explicit DallasTemperature(OneWire* bus) : bus_(bus) {}
    void begin() {}
    void setWaitForConversion(bool enabled) { waitForConversion_ = enabled; }
    void requestTemperatures();
    float getTempC(const uint8_t*) const;
    bool getAddress(uint8_t*, uint8_t) const { return false; }
    uint8_t getDeviceCount() const { return 0; }

private:
    OneWire* bus_;
    bool waitForConversion_ = true;
};

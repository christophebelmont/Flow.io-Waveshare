#pragma once
#include "IOEndpoint.h"
#include <cmath>

class AnalogActuatorEndpoint final : public IOEndpoint {
public:
    void configure(const char* id, float minimum, float maximum, bool (*write)(void*, float), void* context) {
        id_ = id; minimum_ = minimum; maximum_ = maximum; write_ = write; context_ = context;
        value_ = {}; value_.valueType = IO_EP_VALUE_FLOAT;
    }
    const char* id() const override { return id_; }
    IOEndpointType type() const override { return IO_EP_ANALOG_ACTUATOR; }
    uint8_t capabilities() const override { return IO_CAP_READ | IO_CAP_WRITE; }
    bool read(IOEndpointValue& out) override { out = value_; return true; }
    const IOEndpointValue& value() const { return value_; }
    bool write(const IOEndpointValue& in) override {
        if (!write_ || !in.valid || in.valueType != IO_EP_VALUE_FLOAT || !std::isfinite(in.v.f) ||
            in.v.f < minimum_ || in.v.f > maximum_) return false;
        if (!write_(context_, in.v.f)) return false;
        value_ = in; return true;
    }
private:
    const char* id_ = nullptr;
    float minimum_ = 0, maximum_ = 0;
    bool (*write_)(void*, float) = nullptr;
    void* context_ = nullptr;
    IOEndpointValue value_{};
};

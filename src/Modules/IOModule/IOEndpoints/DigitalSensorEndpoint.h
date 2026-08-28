#pragma once
/**
 * @file DigitalSensorEndpoint.h
 * @brief Typed bool sensor endpoint.
 */

#include "Modules/IOModule/IOEndpoints/IOEndpoint.h"

class DigitalSensorEndpoint : public IOEndpoint {
public:
    explicit DigitalSensorEndpoint(const char* endpointId, uint8_t valueType = IO_EP_VALUE_BOOL);

    const char* id() const override { return endpointId_; }
    IOEndpointType type() const override { return IO_EP_DIGITAL_SENSOR; }
    uint8_t capabilities() const override { return IO_CAP_READ; }

    bool read(IOEndpointValue& out) override;
    bool write(const IOEndpointValue&) override { return false; }

    void update(bool on, bool valid, uint32_t timestampMs);
    void updateFloat(float value, bool valid, uint32_t timestampMs);
    void updateCount(int32_t count, bool valid, uint32_t timestampMs);

private:
    const char* endpointId_ = nullptr;
    IOEndpointValue value_{};
};

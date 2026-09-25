#pragma once
/**
 * @file IORegistry.h
 * @brief Static registry for all endpoints.
 */

#include <stdint.h>
#include <string.h>
#include "Modules/IOModule/IOModuleDataModel.h"
#include "Modules/IOModule/IOEndpoints/IOEndpoint.h"

constexpr uint8_t IO_REGISTRY_MAX_ENDPOINTS = IO_MAX_ENDPOINTS;

class IORegistry {
public:
    bool add(IOEndpoint* endpoint, uint16_t numericId);

    uint8_t count() const { return count_; }
    IOEndpoint* at(uint8_t i) const;

    bool read(uint8_t index, IOEndpointValue& out) const;
    bool write(uint8_t index, const IOEndpointValue& in) const;

private:
    IOEndpoint* endpoints_[IO_REGISTRY_MAX_ENDPOINTS] = {nullptr};
    uint8_t count_ = 0;
};

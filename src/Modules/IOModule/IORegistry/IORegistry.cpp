/**
 * @file IORegistry.cpp
 * @brief Implementation file.
 */

#include "IORegistry.h"

bool IORegistry::add(IOEndpoint* endpoint, uint16_t numericId)
{
    if (!endpoint) return false;
    if (count_ >= IO_REGISTRY_MAX_ENDPOINTS) return false;
    endpoint->runtimeIndex = count_;
    endpoint->numericId = numericId;
    endpoints_[count_++] = endpoint;
    return true;
}

IOEndpoint* IORegistry::at(uint8_t i) const
{
    if (i >= count_) return nullptr;
    return endpoints_[i];
}

bool IORegistry::read(uint8_t index, IOEndpointValue& out) const
{
    IOEndpoint* e = at(index);
    if (!e) return false;
    return e->read(out);
}

bool IORegistry::write(uint8_t index, const IOEndpointValue& in) const
{
    IOEndpoint* e = at(index);
    if (!e) return false;
    return e->write(in);
}

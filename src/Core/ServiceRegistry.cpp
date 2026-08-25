/**
 * @file ServiceRegistry.cpp
 * @brief Implementation file.
 */
#include "ServiceRegistry.h"
#include "Core/Log.h"
#include "Core/LogModuleIds.h"

bool ServiceRegistry::add(ServiceId id, const void* service) {
    if (!isValidServiceId(id)) {
        Log::error((LogModuleId)LogModuleIdValue::Core, "service add failed: invalid id=%u",
                   (unsigned)serviceIdIndex(id));
        return false;
    }

    if (service == nullptr) {
        Log::error((LogModuleId)LogModuleIdValue::Core, "service add failed: %s has null pointer",
                   toString(id));
        return false;
    }

    if (count_ >= kServiceIdCount) {
        Log::error((LogModuleId)LogModuleIdValue::Core, "service add failed: registry full (%u)",
                   (unsigned)kServiceIdCount);
        return false;
    }

    const uint8_t index = serviceIdIndex(id);
    if (slots_[index] != nullptr) {
        Log::error((LogModuleId)LogModuleIdValue::Core, "service add failed: duplicate id=%s",
                   toString(id));
        return false;
    }

    slots_[index] = service;
    ++count_;
    return true;
}

bool ServiceRegistry::has(ServiceId id) const
{
    if (!isValidServiceId(id)) return false;
    return slots_[serviceIdIndex(id)] != nullptr;
}

void* ServiceRegistry::getRaw(ServiceId id)
{
    return const_cast<void*>(static_cast<const ServiceRegistry*>(this)->getRaw(id));
}

const void* ServiceRegistry::getRaw(ServiceId id) const
{
    if (!isValidServiceId(id)) return nullptr;
    return slots_[serviceIdIndex(id)];
}

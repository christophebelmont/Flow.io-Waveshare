#pragma once
/**
 * @file ServiceRegistry.h
 * @brief Typed service registry for cross-module access.
 */
#include <stdint.h>
#include "ServiceId.h"

/**
 * @brief Registry of services keyed by ServiceId.
 */
class ServiceRegistry {
public:
    /** @brief Register a service pointer under a strong id. */
    bool add(ServiceId id, const void* service);
    /** @brief Check whether a service id is currently registered. */
    bool has(ServiceId id) const;
    /** @brief Fetch a raw service pointer by id. */
    void* getRaw(ServiceId id);
    /** @brief Fetch a raw service pointer by id. */
    const void* getRaw(ServiceId id) const;

    /** @brief Fetch a typed service pointer by id. */
    template<typename T>
    T* get(ServiceId id) {
        return reinterpret_cast<T*>(getRaw(id));
    }

    /** @brief Fetch a typed service pointer by id. */
    template<typename T>
    const T* get(ServiceId id) const {
        return reinterpret_cast<const T*>(getRaw(id));
    }

private:
    const void* slots_[kServiceIdCount]{};
    uint8_t count_ = 0;
};

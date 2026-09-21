#pragma once
#include "Core/Services/PoolActuatorTypes.h"
#include <stddef.h>
constexpr size_t POOL_DRIVER_CONFIG_BYTES = 1536;
/** Parses one complete, versionless configuration. No migration or legacy aliases. */
bool parsePoolDriverConfig(const char* json, PoolDriverConfig& out, char* error, size_t errorSize);
bool serializePoolDriverConfig(const PoolDriverConfig& config, char* out, size_t size);

#pragma once
/**
 * @file IFlowCfg.h
 * @brief Legacy-compatible contract used by optional configuration/runtime routes.
 */

#include <stddef.h>
#include <stdint.h>

#include "Core/RuntimeUi.h"

enum class FlowStatusDomain : uint8_t {
    System = 1,
    Wifi = 2,
    Mqtt = 3,
    I2c = 4,
    Pool = 5,
    Alarm = 6
};

struct FlowCfgRemoteService {
    bool (*isReady)(void* ctx);
    bool (*setPaused)(void* ctx, bool paused);
    bool (*listModulesJson)(void* ctx, char* out, size_t outLen);
    bool (*listChildrenJson)(void* ctx, const char* prefix, char* out, size_t outLen);
    bool (*getModuleJson)(void* ctx, const char* module, char* out, size_t outLen, bool* truncated);
    bool (*runtimeStatusDomainJson)(void* ctx, FlowStatusDomain domain, char* out, size_t outLen);
    bool (*runtimeStatusJson)(void* ctx, char* out, size_t outLen);
    bool (*runtimeAlarmSnapshotJson)(void* ctx, char* out, size_t outLen);
    bool (*runtimeUiValues)(void* ctx, const RuntimeUiId* ids, uint8_t count, uint8_t* out, size_t outLen, size_t* writtenOut);
    bool (*applyPatchJson)(void* ctx, const char* patch, char* out, size_t outLen);
    void* ctx;
};

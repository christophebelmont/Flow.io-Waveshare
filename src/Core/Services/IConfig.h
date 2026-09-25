#pragma once
/**
 * @file IConfig.h
 * @brief Config store service interface.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <atomic>

/** Stable producer-owned receipt; must outlive a queued request. */
struct PersistenceReceipt {
    enum Status : uint32_t { Idle, Pending, Succeeded, Failed };
    std::atomic<uint32_t> status{Idle};
};

/** @brief Service interface for config JSON import/export. */
struct ConfigStoreService {
    bool (*applyJson)(void* ctx, const char* json);
    void (*toJson)(void* ctx, char* out, size_t outLen);
    bool (*toJsonModule)(void* ctx, const char* module, char* out, size_t outLen, bool* truncated);
    uint8_t (*listModules)(void* ctx, const char** out, uint8_t max);
    bool (*erase)(void* ctx);
    bool (*readRuntimeBlob)(void* ctx, const char* key, void* out, size_t outLen, size_t* actualLen);
    bool (*writeRuntimeBlob)(void* ctx, const char* key, const void* value, size_t len);
    bool (*eraseKey)(void* ctx, const char* key);
    bool (*writeRuntimeBlobAsync)(void* ctx, const char* key, const void* value, size_t len);
    bool (*eraseKeyAsync)(void* ctx, const char* key);
    bool (*persistFloatAsync)(void* ctx,
                              const char* key,
                              float value,
                              const char* moduleName,
                              uint8_t moduleId,
                              uint8_t localBranchId);
    void* ctx;
    bool (*writeRuntimeBlobTracked)(void* ctx, const char* key, const void* value,
                                    size_t len, PersistenceReceipt* receipt) = nullptr;
};

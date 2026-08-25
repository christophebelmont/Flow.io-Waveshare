#pragma once
/**
 * @file BufferUsageTracker.h
 * @brief Peak-usage tracker for large static/heap buffers.
 */

#include <stddef.h>
#include <stdint.h>

#ifndef FLOW_ENABLE_BUFFER_USAGE_TRACKING
#define FLOW_ENABLE_BUFFER_USAGE_TRACKING 1
#endif

enum class TrackedBufferId : uint8_t {
    ConfigMetaTable = 0,
    ConfigApplyJsonDoc,
    LogHubQueue,
    LogHubModules,
    EventBusRuntime,
    MqttRxQueueStorage,
    MqttJobsAndQueues,
    MqttPayloadBuf,
    MqttReplyBuf,
    MqttAckMessages,
    MqttCmdDoc,
    MqttCfgDoc,
    IoHaValueTplTable,
    IoHaSwitchPayloadOnTable,
    IoHaSwitchPayloadOffTable,
    PoolDeviceRuntimePersistTable,
    PoolDeviceSlots,
    HaEntityTables,
    ConfigMenuHeap,
    WifiScanEntries,
    Count
};

struct TrackedBufferSnapshot {
    const char* name = nullptr;
    const char* multiplicity = nullptr;
    uint32_t capacity = 0;
    uint32_t peakUsed = 0;
    char source[24] = {0};
};

class BufferUsageTracker {
public:
#if FLOW_ENABLE_BUFFER_USAGE_TRACKING
    static void note(TrackedBufferId id,
                     size_t used,
                     size_t capacity,
                     const char* source = nullptr,
                     const char* preview = nullptr);

    static void noteFromISR(TrackedBufferId id,
                            size_t used,
                            size_t capacity,
                            const char* source = nullptr,
                            const char* preview = nullptr);

    static size_t snapshot(TrackedBufferSnapshot* out, size_t maxCount);
#else
    static inline void note(TrackedBufferId,
                            size_t,
                            size_t,
                            const char* = nullptr,
                            const char* = nullptr) {}

    static inline void noteFromISR(TrackedBufferId,
                                   size_t,
                                   size_t,
                                   const char* = nullptr,
                                   const char* = nullptr) {}

    static inline size_t snapshot(TrackedBufferSnapshot*, size_t) { return 0U; }
#endif
};

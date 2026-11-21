#pragma once
/**
 * @file PsramJsonDocument.h
 * @brief ArduinoJson 6 document whose JSON pool is explicitly in external PSRAM.
 *
 * Opt-in only: never change the allocator of network, DMA, FreeRTOS stack
 * or other latency-critical buffers. A missing PSRAM allocation returns a
 * zero-capacity document; callers must check capacity() before parsing.
 */
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stddef.h>

struct FlowPsramJsonAllocator {
    void* allocate(size_t size) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    void deallocate(void* pointer) {
        heap_caps_free(pointer);
    }

    void* reallocate(void* pointer, size_t size) {
        return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};

using PsramJsonDocument = BasicJsonDocument<FlowPsramJsonAllocator>;

#pragma once

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Pools are PSRAM-only. Allocation failures are reported by ArduinoJson;
// callers must check capacity/deserialization before using the document.
struct SpiRamJsonAllocator {
    void* allocate(size_t size)
    {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    void deallocate(void* pointer)
    {
        heap_caps_free(pointer);
    }

    void* reallocate(void* pointer, size_t size)
    {
        return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};

// Keep the owning document alive for as long as any JSON view is in use.
using SpiRamJsonDocument = ArduinoJson::BasicJsonDocument<SpiRamJsonAllocator>;

#pragma once
#include <cstdlib>
#include <cstring>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
inline bool fakePsramAvailable = true;
inline bool fakeInternalAvailable = true;
inline unsigned fakeHeapAllocations = 0;
inline void* heap_caps_malloc(size_t size, unsigned caps) {
    ++fakeHeapAllocations;
    if ((caps & MALLOC_CAP_SPIRAM) && !fakePsramAvailable) return nullptr;
    if ((caps & MALLOC_CAP_INTERNAL) && !fakeInternalAvailable) return nullptr;
    return std::malloc(size);
}
inline void heap_caps_free(void* ptr) { std::free(ptr); }

inline void* heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    auto* memory = heap_caps_malloc(count * size, caps);
    if (memory) std::memset(memory, 0, count * size);
    return memory;
}
inline void* heap_caps_realloc(void* memory, size_t size, unsigned caps) {
    ++fakeHeapAllocations;
    if ((caps & MALLOC_CAP_SPIRAM) && !fakePsramAvailable) return nullptr;
    return std::realloc(memory, size);
}

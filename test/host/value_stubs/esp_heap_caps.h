#pragma once
#include <cstdlib>
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

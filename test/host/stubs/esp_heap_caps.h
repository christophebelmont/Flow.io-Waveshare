#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline void* heap_caps_malloc(size_t n, int) { return std::malloc(n); }
inline void heap_caps_free(void* p) { std::free(p); }
inline void* heap_caps_realloc(void* p, size_t n, int) { return std::realloc(p,n); }

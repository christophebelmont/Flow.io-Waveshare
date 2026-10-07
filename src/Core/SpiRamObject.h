#pragma once

#include <esp_heap_caps.h>
#include <memory>
#include <new>
#include <utility>

// Typed ownership for application data that must never fall back to internal RAM.
// Allocate outside critical sections and keep locks / ISR data in internal RAM.
template<class T>
struct SpiRamDeleter {
    void operator()(T* object) const noexcept
    {
        if (!object) return;
        object->~T();
        heap_caps_free(object);
    }
};

template<class T>
using SpiRamPtr = std::unique_ptr<T, SpiRamDeleter<T>>;

template<class T, class... Args>
SpiRamPtr<T> makeSpiRamObject(Args&&... args)
{
    void* storage = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) return {};
    return SpiRamPtr<T>(new (storage) T(std::forward<Args>(args)...));
}

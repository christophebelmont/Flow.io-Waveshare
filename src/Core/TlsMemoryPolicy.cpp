/**
 * @file TlsMemoryPolicy.cpp
 * @brief Process-wide mbedTLS allocator backed primarily by PSRAM.
 */

#include "Core/TlsMemoryPolicy.h"

#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <stdint.h>

namespace {

void* tlsCalloc_(size_t count, size_t size)
{
    if (size != 0U && count > (SIZE_MAX / size)) return nullptr;

    return heap_caps_calloc_prefer(count,
                                   size,
                                   2U,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void tlsFree_(void* pointer)
{
    heap_caps_free(pointer);
}

}  // namespace

namespace TlsMemoryPolicy {

bool installPsramPreferred()
{
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == 0U) {
        return false;
    }
    return mbedtls_platform_set_calloc_free(&tlsCalloc_, &tlsFree_) == 0;
}

}  // namespace TlsMemoryPolicy

#pragma once
/**
 * @file ILocale.h
 * @brief Locale service interface.
 */

#include <stdint.h>

/** @brief Service interface exposing the normalized runtime language. */
struct LocaleService {
    const char* (*language)(void* ctx);
    uint32_t (*generation)(void* ctx);
    void* ctx;
};

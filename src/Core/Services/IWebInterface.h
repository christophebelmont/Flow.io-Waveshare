#pragma once
/**
 * @file IWebInterface.h
 * @brief Web interface runtime control service.
 */

#include <stddef.h>
#include <stdint.h>

struct WebInterfaceHealth {
    uint32_t snapshotMs;
    uint32_t lastLoopMs;
    uint32_t lastHttpActivityMs;
    uint32_t lastWsActivityMs;
    uint16_t wsSerialClients;
    uint16_t wsLogClients;
    bool started;
    bool paused;
};

struct WebInterfaceService {
    bool (*setPaused)(void* ctx, bool paused);
    bool (*isPaused)(void* ctx);
    bool (*getHealth)(void* ctx, WebInterfaceHealth* out);
    void* ctx;
};

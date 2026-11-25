#pragma once
/**
 * @file IFirmwareUpdate.h
 * @brief Firmware update service interface.
 */

#include <stddef.h>
#include <stdint.h>

#include "Core/Services/IHmi.h"

enum class FirmwareUpdateTarget : uint8_t {
    Nextion = 2,
    Waveshare = 3,
    Spiffs = 4
};

enum class FirmwareManifestCheckState : uint8_t {
    Idle = 0,
    Queued,
    Downloading,
    Ready,
    Error
};

struct FirmwareManifestCheckSnapshot {
    uint32_t requestId = 0;
    FirmwareManifestCheckState state = FirmwareManifestCheckState::Idle;
    uint32_t updatedAtMs = 0;
    size_t payloadLen = 0;
    char manifestUrl[192] = {0};
    char message[120] = {0};
    bool nextionDisplayDetected = false;
    bool nextionArtifactSelected = false;
    char nextionDisplayModel[HMI_DISPLAY_MODEL_TEXT_MAX] = {0};
    char nextionDisplayCompatibility[HMI_DISPLAY_MODEL_TEXT_MAX] = {0};
    char nextionArtifactPath[128] = {0};
    char nextionArtifactVersion[HMI_DISPLAY_VERSION_TEXT_MAX] = {0};
    char nextionArtifactUrl[192] = {0};
    uint32_t nextionArtifactSize = 0U;
};

// Lightweight, lock-copied progress snapshot for local TFT rendering.
// Unlike statusJson(), this callback allocates no JSON document or String.
enum class FirmwareUpdateDisplayState : uint8_t {
    Idle = 0, Queued, Downloading, Flashing, Rebooting, Done, Error
};

struct FirmwareUpdateDisplaySnapshot {
    FirmwareUpdateDisplayState state = FirmwareUpdateDisplayState::Idle;
    FirmwareUpdateTarget target = FirmwareUpdateTarget::Waveshare;
    uint32_t operationId = 0U;
    uint8_t progress = 0U;        // Global progress; may stay at zero during PSRAM download.
    uint8_t stageProgress = 0U;   // Accurate percentage for the current OTA phase.
    char stage[28] = {0};         // Stable machine-readable stage, never translated.
    char message[120] = {0};      // Existing diagnostic message.
};

// ZIP upload staging / flash progress. Readable without blocking AsyncTCP
// on the Flash mutex. Transaction ID prevents stale browser polling.
enum class FirmwareLocalImagePhase : uint8_t {
    Idle = 0, Receiving, Queued, Flashing, Verified, Error
};
struct FirmwareLocalImageProgress {
    uint32_t transactionId = 0U;
    FirmwareUpdateTarget target = FirmwareUpdateTarget::Waveshare;
    FirmwareLocalImagePhase phase = FirmwareLocalImagePhase::Idle;
    uint8_t percent = 0U;
    uint32_t receivedBytes = 0U;
    uint32_t totalBytes = 0U;
    char reason[120]{};
};

struct FirmwareUpdateService {
    bool (*start)(void* ctx,
                  FirmwareUpdateTarget target,
                  const char* url,
                  uint32_t* operationIdOut,
                  char* errOut,
                  size_t errOutLen);
    bool (*statusJson)(void* ctx, char* out, size_t outLen);
    bool (*isBusy)(void* ctx);
    bool (*configJson)(void* ctx, char* out, size_t outLen);
    bool (*startManifestCheck)(void* ctx, uint32_t* requestIdOut, char* errOut, size_t errOutLen);
    bool (*manifestCheckStatus)(void* ctx, uint32_t requestId, FirmwareManifestCheckSnapshot* out);
    bool (*copyManifestResult)(void* ctx,
                               uint32_t requestId,
                               char* out,
                               size_t outLen,
                               size_t* copiedLenOut);
    bool (*setConfig)(void* ctx,
                      const char* updateHost,
                      const char* updatePath,
                      char* errOut,
                      size_t errOutLen);
    bool (*beginLocalRelease)(void* ctx,
                              const char* manifestJson,
                              size_t manifestLen,
                              uint32_t* transactionIdOut,
                              char* errOut,
                              size_t errOutLen);
    bool (*beginLocalImage)(void* ctx,
                            uint32_t transactionId,
                            FirmwareUpdateTarget target,
                            size_t totalSize,
                            char* errOut,
                            size_t errOutLen);
    bool (*writeLocalImage)(void* ctx,
                            uint32_t transactionId,
                            FirmwareUpdateTarget target,
                            const uint8_t* data,
                            size_t len,
                            size_t offset,
                            char* errOut,
                            size_t errOutLen);
    bool (*endLocalImage)(void* ctx,
                          uint32_t transactionId,
                          FirmwareUpdateTarget target,
                          char* errOut,
                          size_t errOutLen);
    bool (*commitLocalRelease)(void* ctx,
                               uint32_t transactionId,
                               char* errOut,
                               size_t errOutLen);
    bool (*abortLocalRelease)(void* ctx,
                              uint32_t transactionId,
                              char* errOut,
                              size_t errOutLen);
    void* ctx;
    // Optional: no heap use, suitable for low-frequency display polling.
    bool (*displaySnapshot)(void* ctx, FirmwareUpdateDisplaySnapshot* out);
    // Optional lock-copied snapshot for the asynchronously flashed ZIP image.
    bool (*localImageProgress)(void* ctx,
                               uint32_t transactionId,
                               FirmwareUpdateTarget target,
                               FirmwareLocalImageProgress* out);
};

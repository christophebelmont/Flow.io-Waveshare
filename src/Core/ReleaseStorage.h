#pragma once
/**
 * @file ReleaseStorage.h
 * @brief Couples the running OTA application slot with its immutable SPIFFS slot.
 */

#include <stdint.h>

#include <FS.h>

enum class ReleaseSlot : uint8_t {
    A = 0,
    B = 1
};

namespace ReleaseStorage {

constexpr const char* kSlotALabel = "spiffs0";
constexpr const char* kSlotBLabel = "spiffs1";
constexpr const char* kRuntimeLabel = "runtime";

/** Mount the release filesystem selected from the running OTA partition. */
bool beginReleaseFilesystem();

/** Mount the mutable runtime filesystem. Formatting is allowed only here. */
bool beginRuntimeFilesystem();

ReleaseSlot runningSlot();
ReleaseSlot inactiveSlot();
const char* filesystemLabel(ReleaseSlot slot);
const char* applicationLabel(ReleaseSlot slot);

bool releaseReady();
bool runtimeReady();
fs::FS& releaseFilesystem();
fs::FS& runtimeFilesystem();

/** Validate the minimum assets required before accepting a pending OTA app. */
bool validateReleaseFilesystem();

/** Validate release contents mounted through any filesystem instance. */
bool validateReleaseFilesystem(fs::FS& filesystem,
                               const char* expectedVersion,
                               const char* expectedHardware);

/** Mark a pending OTA image valid after the application startup completed. */
bool confirmPendingApplication();

}  // namespace ReleaseStorage

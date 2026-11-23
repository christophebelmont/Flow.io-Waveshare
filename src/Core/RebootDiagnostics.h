#pragma once
/**
 * @file RebootDiagnostics.h
 * @brief Best-effort reboot and OTA progress breadcrumbs across software reset.
 *
 * OTA progress is stored in RTC no-init RAM with no flash writes on the
 * download/write hot path. Lost on power removal (RTC). A one-shot NVS copy
 * is made ONLY immediately before an intentional restart, then consumed on
 * next boot. Not a replacement for panic dumps or serial logs.
 */
#include <stdint.h>

namespace RebootDiagnostics {

enum class Origin : uint32_t {
    None = 0,
    OtaRelease = 1,
    OtaNextion = 2,
    System = 3,
    FactoryReset = 4
};

enum class OtaStage : uint32_t {
    None = 0,
    Manifest = 1,
    Spiffs = 2,
    Firmware = 3,
    VerifySpiffs = 4,
    VerifyFirmware = 5,
    Commit = 6,
    SpiffsRamDownload = 7,
    FirmwareRamDownload = 8,
    SpiffsRamVerify = 9,
    FirmwareRamVerify = 10
};

// Records a checkpoint without altering OTA state or doing flash I/O.
void noteOta(uint32_t operationId, OtaStage stage,
             uint32_t receivedBytes = 0U, uint32_t totalBytes = 0U);

// Call only immediately before an intentional ESP/IDF restart.
void markPlanned(Origin origin, uint32_t operationId = 0U);

// Reports the previous boot once, and clears the marker for this boot.
void reportPreviousBoot();

} // namespace RebootDiagnostics

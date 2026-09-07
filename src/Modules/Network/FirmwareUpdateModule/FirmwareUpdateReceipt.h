#pragma once
/**
 * @file FirmwareUpdateReceipt.h
 * @brief Persistent firmware update result shared across a device reboot.
 */

#include <stdint.h>

#include "Core/Services/IFirmwareUpdate.h"

enum class FirmwareUpdateReceiptState : uint8_t {
    Running = 1,
    RebootPending,
    Succeeded,
    Interrupted,
    Failed
};

struct FirmwareUpdateReceipt {
    uint32_t magic = 0U;
    uint16_t schemaVersion = 0U;
    FirmwareUpdateReceiptState state = FirmwareUpdateReceiptState::Running;
    FirmwareUpdateTarget target = FirmwareUpdateTarget::Waveshare;
    uint32_t operationId = 0U;
    uint32_t checksum = 0U;
};

FirmwareUpdateReceipt makeFirmwareUpdateReceipt(FirmwareUpdateTarget target,
                                                uint32_t operationId,
                                                FirmwareUpdateReceiptState state);
bool firmwareUpdateReceiptIsValid(const FirmwareUpdateReceipt& receipt);
bool finalizeFirmwareUpdateReceiptAfterBoot(FirmwareUpdateReceipt* receipt);
const char* firmwareUpdateReceiptStateName(FirmwareUpdateReceiptState state);

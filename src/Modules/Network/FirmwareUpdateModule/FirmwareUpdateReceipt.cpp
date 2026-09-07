/**
 * @file FirmwareUpdateReceipt.cpp
 * @brief Persistent firmware update result implementation.
 */

#include "FirmwareUpdateReceipt.h"

namespace {

constexpr uint32_t kReceiptMagic = 0x46575550UL;  // "FWUP"
constexpr uint16_t kReceiptSchemaVersion = 1U;
constexpr uint32_t kFnvOffsetBasis = 2166136261UL;
constexpr uint32_t kFnvPrime = 16777619UL;

uint32_t appendByte_(uint32_t hash, uint8_t value)
{
    return (hash ^ value) * kFnvPrime;
}

uint32_t appendUint16_(uint32_t hash, uint16_t value)
{
    hash = appendByte_(hash, (uint8_t)(value & 0xFFU));
    return appendByte_(hash, (uint8_t)((value >> 8U) & 0xFFU));
}

uint32_t appendUint32_(uint32_t hash, uint32_t value)
{
    for (uint8_t shift = 0U; shift < 32U; shift += 8U) {
        hash = appendByte_(hash, (uint8_t)((value >> shift) & 0xFFU));
    }
    return hash;
}

uint32_t checksum_(const FirmwareUpdateReceipt& receipt)
{
    uint32_t hash = kFnvOffsetBasis;
    hash = appendUint32_(hash, receipt.magic);
    hash = appendUint16_(hash, receipt.schemaVersion);
    hash = appendByte_(hash, static_cast<uint8_t>(receipt.state));
    hash = appendByte_(hash, static_cast<uint8_t>(receipt.target));
    return appendUint32_(hash, receipt.operationId);
}

bool targetIsValid_(FirmwareUpdateTarget target)
{
    return target == FirmwareUpdateTarget::Nextion ||
           target == FirmwareUpdateTarget::Waveshare ||
           target == FirmwareUpdateTarget::Spiffs;
}

bool stateIsValid_(FirmwareUpdateReceiptState state)
{
    return state >= FirmwareUpdateReceiptState::Running &&
           state <= FirmwareUpdateReceiptState::Failed;
}

}  // namespace

FirmwareUpdateReceipt makeFirmwareUpdateReceipt(FirmwareUpdateTarget target,
                                                uint32_t operationId,
                                                FirmwareUpdateReceiptState state)
{
    FirmwareUpdateReceipt receipt{};
    receipt.magic = kReceiptMagic;
    receipt.schemaVersion = kReceiptSchemaVersion;
    receipt.state = state;
    receipt.target = target;
    receipt.operationId = operationId;
    receipt.checksum = checksum_(receipt);
    return receipt;
}

bool firmwareUpdateReceiptIsValid(const FirmwareUpdateReceipt& receipt)
{
    return receipt.magic == kReceiptMagic &&
           receipt.schemaVersion == kReceiptSchemaVersion &&
           receipt.operationId != 0U &&
           targetIsValid_(receipt.target) &&
           stateIsValid_(receipt.state) &&
           receipt.checksum == checksum_(receipt);
}

bool finalizeFirmwareUpdateReceiptAfterBoot(FirmwareUpdateReceipt* receipt)
{
    if (!receipt || !firmwareUpdateReceiptIsValid(*receipt)) return false;

    FirmwareUpdateReceiptState finalState = receipt->state;
    if (receipt->state == FirmwareUpdateReceiptState::RebootPending) {
        finalState = FirmwareUpdateReceiptState::Succeeded;
    } else if (receipt->state == FirmwareUpdateReceiptState::Running) {
        finalState = FirmwareUpdateReceiptState::Interrupted;
    } else {
        return false;
    }

    *receipt = makeFirmwareUpdateReceipt(receipt->target, receipt->operationId, finalState);
    return true;
}

const char* firmwareUpdateReceiptStateName(FirmwareUpdateReceiptState state)
{
    switch (state) {
        case FirmwareUpdateReceiptState::Running: return "running";
        case FirmwareUpdateReceiptState::RebootPending: return "reboot_pending";
        case FirmwareUpdateReceiptState::Succeeded: return "succeeded";
        case FirmwareUpdateReceiptState::Interrupted: return "interrupted";
        case FirmwareUpdateReceiptState::Failed: return "failed";
        default: return "unknown";
    }
}

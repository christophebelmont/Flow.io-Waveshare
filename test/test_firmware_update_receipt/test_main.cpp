#include <unity.h>

#include "Modules/Network/FirmwareUpdateModule/FirmwareUpdateReceipt.h"

void setUp() {}
void tearDown() {}

void test_receipt_round_trip_is_valid()
{
    const FirmwareUpdateReceipt receipt = makeFirmwareUpdateReceipt(
        FirmwareUpdateTarget::Nextion,
        42U,
        FirmwareUpdateReceiptState::Running);

    TEST_ASSERT_TRUE(firmwareUpdateReceiptIsValid(receipt));
    TEST_ASSERT_EQUAL_UINT32(42U, receipt.operationId);
    TEST_ASSERT_EQUAL_STRING("running", firmwareUpdateReceiptStateName(receipt.state));
}

void test_modified_receipt_is_rejected()
{
    FirmwareUpdateReceipt receipt = makeFirmwareUpdateReceipt(
        FirmwareUpdateTarget::Spiffs,
        7U,
        FirmwareUpdateReceiptState::RebootPending);
    receipt.operationId = 8U;

    TEST_ASSERT_FALSE(firmwareUpdateReceiptIsValid(receipt));
}

void test_reboot_pending_becomes_succeeded_after_boot()
{
    FirmwareUpdateReceipt receipt = makeFirmwareUpdateReceipt(
        FirmwareUpdateTarget::Waveshare,
        100U,
        FirmwareUpdateReceiptState::RebootPending);

    TEST_ASSERT_TRUE(finalizeFirmwareUpdateReceiptAfterBoot(&receipt));
    TEST_ASSERT_TRUE(firmwareUpdateReceiptIsValid(receipt));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FirmwareUpdateReceiptState::Succeeded,
                            (uint8_t)receipt.state);
}

void test_running_becomes_interrupted_after_boot()
{
    FirmwareUpdateReceipt receipt = makeFirmwareUpdateReceipt(
        FirmwareUpdateTarget::Nextion,
        101U,
        FirmwareUpdateReceiptState::Running);

    TEST_ASSERT_TRUE(finalizeFirmwareUpdateReceiptAfterBoot(&receipt));
    TEST_ASSERT_TRUE(firmwareUpdateReceiptIsValid(receipt));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FirmwareUpdateReceiptState::Interrupted,
                            (uint8_t)receipt.state);
}

void test_terminal_receipt_is_not_changed_after_boot()
{
    FirmwareUpdateReceipt receipt = makeFirmwareUpdateReceipt(
        FirmwareUpdateTarget::Nextion,
        102U,
        FirmwareUpdateReceiptState::Failed);

    TEST_ASSERT_FALSE(finalizeFirmwareUpdateReceiptAfterBoot(&receipt));
    TEST_ASSERT_TRUE(firmwareUpdateReceiptIsValid(receipt));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FirmwareUpdateReceiptState::Failed,
                            (uint8_t)receipt.state);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_receipt_round_trip_is_valid);
    RUN_TEST(test_modified_receipt_is_rejected);
    RUN_TEST(test_reboot_pending_becomes_succeeded_after_boot);
    RUN_TEST(test_running_becomes_interrupted_after_boot);
    RUN_TEST(test_terminal_receipt_is_not_changed_after_boot);
    return UNITY_END();
}

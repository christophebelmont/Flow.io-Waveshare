#include <unity.h>

#include "Modules/HMIModule/Drivers/NextionDisplayIdentity.h"

#include <string.h>

void setUp() {}
void tearDown() {}

void test_connect_response_extracts_resistive_model()
{
    HmiDisplayIdentity identity{};
    TEST_ASSERT_TRUE(parseNextionConnectResponse(
        "comok 1,38024-0,NX8048P050_011R,99,61488,D264B8204F0E1828,16777216",
        identity));
    TEST_ASSERT_EQUAL_STRING("NX8048P050_011R", identity.model);
    TEST_ASSERT_EQUAL_STRING("NX8048P050_011", identity.compatibility);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)HmiDisplayTouchType::Resistive, (uint8_t)identity.touchType);
    TEST_ASSERT_EQUAL_UINT16(99U, identity.deviceFirmwareVersion);
}

void test_capacitive_and_resistive_share_compatibility()
{
    HmiDisplayIdentity resistive{};
    HmiDisplayIdentity capacitive{};
    TEST_ASSERT_TRUE(parseNextionDisplayModel("NX8048P050-011R", resistive));
    TEST_ASSERT_TRUE(parseNextionDisplayModel("nx8048p050_011c", capacitive));
    TEST_ASSERT_EQUAL_STRING(resistive.compatibility, capacitive.compatibility);
    TEST_ASSERT_EQUAL_STRING("NX8048P050_011", capacitive.compatibility);
}

void test_variant_suffix_is_preserved()
{
    HmiDisplayIdentity resistive{};
    HmiDisplayIdentity capacitive{};
    TEST_ASSERT_TRUE(parseNextionDisplayModel("NX4827P043-011R-Y", resistive));
    TEST_ASSERT_TRUE(parseNextionDisplayModel("NX4827P043-011C-Y", capacitive));
    TEST_ASSERT_EQUAL_STRING("NX4827P043_011_Y", resistive.compatibility);
    TEST_ASSERT_EQUAL_STRING(resistive.compatibility, capacitive.compatibility);
}

void test_no_touch_model_remains_distinct()
{
    HmiDisplayIdentity identity{};
    TEST_ASSERT_TRUE(parseNextionDisplayModel("NX4832K035_011N", identity));
    TEST_ASSERT_EQUAL_STRING("NX4832K035_011N", identity.compatibility);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)HmiDisplayTouchType::None, (uint8_t)identity.touchType);
}

void test_artifact_filename_uses_touch_neutral_model()
{
    char compatibility[HMI_DISPLAY_MODEL_TEXT_MAX]{};
    char version[HMI_DISPLAY_VERSION_TEXT_MAX]{};
    TEST_ASSERT_TRUE(parseNextionArtifactFilename(
        "FlowIO_Nextion_NX8048P070_011-6.0.0.tft",
        compatibility,
        sizeof(compatibility),
        version,
        sizeof(version)));
    TEST_ASSERT_EQUAL_STRING("NX8048P070_011", compatibility);
    TEST_ASSERT_EQUAL_STRING("6.0.0", version);
}

void test_artifact_filename_rejects_touch_specific_model()
{
    char compatibility[HMI_DISPLAY_MODEL_TEXT_MAX]{};
    char version[HMI_DISPLAY_VERSION_TEXT_MAX]{};
    TEST_ASSERT_FALSE(parseNextionArtifactFilename(
        "FlowIO_Nextion_NX8048P070_011C-6.0.0.tft",
        compatibility,
        sizeof(compatibility),
        version,
        sizeof(version)));
}

void test_numeric_versions_compare_by_component()
{
    TEST_ASSERT_EQUAL_INT(-1, compareNextionVersions("6.0.9", "6.1.0"));
    TEST_ASSERT_EQUAL_INT(0, compareNextionVersions("6.1.0", "6.1.0"));
    TEST_ASSERT_EQUAL_INT(1, compareNextionVersions("10.0.0", "6.9.9"));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_connect_response_extracts_resistive_model);
    RUN_TEST(test_capacitive_and_resistive_share_compatibility);
    RUN_TEST(test_variant_suffix_is_preserved);
    RUN_TEST(test_no_touch_model_remains_distinct);
    RUN_TEST(test_artifact_filename_uses_touch_neutral_model);
    RUN_TEST(test_artifact_filename_rejects_touch_specific_model);
    RUN_TEST(test_numeric_versions_compare_by_component);
    return UNITY_END();
}

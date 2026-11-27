/**
 * @file TFTModuleS3.cpp
 * @brief Local Waveshare ESP32-S3 TFT display.
 */

#include "Core/Values/ValueFormat.h"
#include "Modules/TFTModuleS3/TFTModuleS3.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <new>
#include <ArduinoJson.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include "Modules/TFTModuleS3/FreeSansBold8pt7b.h"
#include <Fonts/FreeSansBold12pt7b.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "Board/BoardSpec.h"
#include "Core/DataKeys.h"
#include "Core/EventBus/EventPayloads.h"
#include "Core/FirmwareVersion.h"
#include "Core/Generated/RuntimeUiAlarmText_Generated.h"
#include "Core/Generated/RuntimeUiManifest_Generated.h"
#include "Core/Services/IAlarm.h"
#include "Core/Services/IIO.h"
#include "Core/SystemLimits.h"
#include "Core/SystemStats.h"
#include "Domain/Pool/PoolIds.h"
#include "Modules/IOModule/IODerivedValueName.h"
#include "Modules/IOModule/IORuntime.h"
#include "Modules/Network/MQTTModule/MQTTRuntime.h"
#include "Modules/Network/WifiModule/WifiRuntime.h"
#include "Modules/PoolDeviceModule/PoolDeviceRuntime.h"
#include "Modules/TFTModuleS3/FlowIoLogoBitmap.h"
#include "Modules/TFTModuleS3/FlowIoWordmarkBitmaps.h"
#include "Modules/TFTModuleS3/FlowIoWaveDarkBitmap.h"
#include "Modules/TFTModuleS3/TftUpdateStages.h"
#include <WiFi.h>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::HMIModule)
#include "Core/ModuleLog.h"

namespace {
constexpr uint8_t kCfgProducerId = 54;
constexpr uint8_t kCfgBranch = 1;
constexpr uint8_t kCfgBranchSensorBase = 16;
constexpr uint8_t kCfgBranchAlarmBase = 32;
constexpr uint16_t kCfgMsgSensorBase = 16U;
constexpr uint16_t kCfgMsgAlarmBase = 32U;
constexpr uint32_t kRenderPeriodMs = 1000U;
constexpr uint32_t kFlowMeterTftRefreshMs = 5000U; // Display only; acquisition remains 1 Hz.
constexpr uint32_t kBacklightTimeoutMs = 60000U;
constexpr uint32_t kStartupSplashHoldMs = 5000U;
constexpr uint32_t kWakeSplashHoldMs = 1000U;
constexpr uint32_t kUpdateRenderMinMs = 250U;  // Keep OTA/network load low.
constexpr uint32_t kUpdateResultHoldMs = 20000U; // Show errors for 20 s.
constexpr uint32_t kPageRotateMs = 10000U;

constexpr uint16_t rgb565_(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

// Only the three rotating dashboard pages use this dark palette.
// OTA/Nextion progress uses its own explicit palette; boot splash stays white.
constexpr uint16_t kColorBg = rgb565_(4, 17, 32);
constexpr uint16_t kColorHeader = rgb565_(5, 20, 37);
constexpr uint16_t kColorText = rgb565_(237, 248, 255);
constexpr uint16_t kColorMuted = rgb565_(135, 160, 181);
constexpr uint16_t kColorWifiOff = rgb565_(41, 65, 87);
constexpr uint16_t kColorOn = rgb565_(27, 205, 127);
constexpr uint16_t kColorOff = rgb565_(111, 130, 148);
constexpr uint16_t kColorAlarmAct = rgb565_(240, 75, 82);
constexpr uint16_t kColorAlarmAck = rgb565_(242, 173, 64);
constexpr uint16_t kColorGaugeOk = rgb565_(32, 196, 101);
constexpr uint16_t kColorCardBg = rgb565_(10, 36, 59);
constexpr uint16_t kColorCardBorder = rgb565_(17, 198, 237);
constexpr uint16_t kColorValue = rgb565_(255, 248, 217);
constexpr uint16_t kColorCheckMark = rgb565_(255, 255, 255);
constexpr int16_t kHeaderH = 32; // Compact shared header for all three rotating pages.
constexpr int16_t kSidePad = 8; // Header underline inset, not card margin.
constexpr int16_t kCardEdge = 1; // Approved TFT left/right/bottom margins.
constexpr int16_t kCardGap = 2;  // Approved gap between columns and rows.
constexpr uint8_t kAlarmSlotCount = 8U;
constexpr uint16_t kColorSelected = rgb565_(60, 208, 250);
constexpr uint8_t kOverviewSelectorRows = 3U;
constexpr int16_t kOverviewSelectorGap = kCardGap;
constexpr int16_t kUiCardRadius = 6;
constexpr uint16_t kColorOverviewModeBg = rgb565_(11, 42, 69);
constexpr uint16_t kColorOverviewPhBg = rgb565_(10, 46, 55);
constexpr uint16_t kColorOverviewTreatmentBg = rgb565_(9, 44, 65);

// Keep the user's configurable card color IDs meaningful under dark mode:
// the existing pastel hue contributes a subtle tint over a navy card.
uint16_t dashboardDarkTint_(uint16_t preset)
{
    const uint8_t r = (uint8_t)((preset >> 11U) & 31U);
    const uint8_t g = (uint8_t)((preset >> 5U) & 63U);
    const uint8_t b = (uint8_t)(preset & 31U);
    return rgb565_((uint8_t)(6U + r * 9U / 31U),
                   (uint8_t)(23U + g * 17U / 63U),
                   (uint8_t)(41U + b * 22U / 31U));
}

constexpr const char* kSensorModuleNames[TFTModuleS3::DashboardSlotCount] = {
    "tft/s3/sensors/slot00",
    "tft/s3/sensors/slot01",
    "tft/s3/sensors/slot02",
    "tft/s3/sensors/slot03",
    "tft/s3/sensors/slot04",
    "tft/s3/sensors/slot05",
    "tft/s3/sensors/slot06",
    "tft/s3/sensors/slot07",
    "tft/s3/sensors/slot08",
    "tft/s3/sensors/slot09",
};

constexpr const char* kSensorEnabledKeys[TFTModuleS3::DashboardSlotCount] = {
    "tfs00en", "tfs01en", "tfs02en", "tfs03en", "tfs04en", "tfs05en", "tfs06en", "tfs07en", "tfs08en", "tfs09en"
};

constexpr const char* kSensorRuntimeIdKeys[TFTModuleS3::DashboardSlotCount] = {
    "tfs00ri", "tfs01ri", "tfs02ri", "tfs03ri", "tfs04ri", "tfs05ri", "tfs06ri", "tfs07ri", "tfs08ri", "tfs09ri"
};

constexpr const char* kSensorLabelKeys[TFTModuleS3::DashboardSlotCount] = {
    "tfs00lb", "tfs01lb", "tfs02lb", "tfs03lb", "tfs04lb", "tfs05lb", "tfs06lb", "tfs07lb", "tfs08lb", "tfs09lb"
};

constexpr const char* kSensorColorIdKeys[TFTModuleS3::DashboardSlotCount] = {
    "tfs00uc", "tfs01uc", "tfs02uc", "tfs03uc", "tfs04uc", "tfs05uc", "tfs06uc", "tfs07uc", "tfs08uc", "tfs09uc"
};

constexpr const char* kAlarmModuleNames[TFTModuleS3::AlarmDashboardSlotCount] = {
    "tft/s3/alarms/slot00",
    "tft/s3/alarms/slot01",
    "tft/s3/alarms/slot02",
    "tft/s3/alarms/slot03",
    "tft/s3/alarms/slot04",
    "tft/s3/alarms/slot05",
    "tft/s3/alarms/slot06",
    "tft/s3/alarms/slot07",
};

constexpr const char* kAlarmEnabledKeys[TFTModuleS3::AlarmDashboardSlotCount] = {
    "tfa00en", "tfa01en", "tfa02en", "tfa03en", "tfa04en", "tfa05en", "tfa06en", "tfa07en"
};

constexpr const char* kAlarmIdKeys[TFTModuleS3::AlarmDashboardSlotCount] = {
    "tfa00id", "tfa01id", "tfa02id", "tfa03id", "tfa04id", "tfa05id", "tfa06id", "tfa07id"
};

constexpr const char* kAlarmLabelKeys[TFTModuleS3::AlarmDashboardSlotCount] = {
    "tfa00lb", "tfa01lb", "tfa02lb", "tfa03lb", "tfa04lb", "tfa05lb", "tfa06lb", "tfa07lb"
};

constexpr const char* kAlarmColorIdKeys[TFTModuleS3::AlarmDashboardSlotCount] = {
    "tfa00uc", "tfa01uc", "tfa02uc", "tfa03uc", "tfa04uc", "tfa05uc", "tfa06uc", "tfa07uc"
};

struct DashboardColorPreset {
    uint8_t id;
    uint16_t rgb565;
};

constexpr RuntimeUiId kDashboardDefaultRuntimeUiIds[TFTModuleS3::DashboardSlotCount] = {
    makeRuntimeUiId(ModuleId::Io, 1),
    makeRuntimeUiId(ModuleId::Io, 2),
    makeRuntimeUiId(ModuleId::Io, 3),
    makeRuntimeUiId(ModuleId::Io, 4),
    makeRuntimeUiId(ModuleId::Io, 5),
    makeRuntimeUiId(ModuleId::Io, 8),
    makeRuntimeUiId(ModuleId::Io, 7),
    makeRuntimeUiId(ModuleId::Io, 6),
    0U,
    0U,
};

constexpr const char* kDashboardDefaultLabels[TFTModuleS3::DashboardSlotCount] = {
    "Eau",
    "Air",
    "pH",
    "ORP",
    "Compteur",
    "BME680",
    "BMP280",
    "PSI",
    "Mesure 9",
    "Mesure 10",
};

constexpr DashboardColorPreset kDashboardColorPresets[] = {
    {0U, rgb565_(230, 239, 255)},
    {1U, rgb565_(229, 248, 252)},
    {2U, rgb565_(232, 250, 239)},
    {3U, rgb565_(240, 234, 254)},
    {4U, rgb565_(228, 246, 250)},
    {5U, rgb565_(227, 247, 254)},
    {6U, rgb565_(234, 248, 253)},
    {7U, rgb565_(254, 240, 232)},
    {8U, rgb565_(252, 231, 239)},
    {9U, rgb565_(255, 240, 225)},
    {10U, rgb565_(255, 247, 217)},
    {11U, rgb565_(237, 248, 231)},
    {12U, rgb565_(240, 250, 230)},
    {13U, rgb565_(245, 238, 255)},
    {14U, rgb565_(235, 238, 255)},
    {15U, rgb565_(238, 247, 255)},
    {16U, rgb565_(244, 250, 222)},
    {17U, rgb565_(255, 233, 228)},
    {18U, rgb565_(249, 238, 232)},
    {19U, rgb565_(241, 244, 248)},
    {20U, rgb565_(255, 255, 255)},
};

constexpr uint8_t kDashboardDefaultColorIds[TFTModuleS3::DashboardSlotCount] = {
    0U,
    1U,
    2U,
    3U,
    4U,
    5U,
    6U,
    7U,
    8U,
    9U,
};

constexpr uint16_t kAlarmDefaultIds[TFTModuleS3::AlarmDashboardSlotCount] = {
    (uint16_t)AlarmId::PoolPsiLow,
    (uint16_t)AlarmId::PoolPsiHigh,
    (uint16_t)AlarmId::PoolPhTankLow,
    (uint16_t)AlarmId::PoolChlorineTankLow,
    (uint16_t)AlarmId::PoolPhPumpMaxUptime,
    (uint16_t)AlarmId::PoolChlorinePumpMaxUptime,
    (uint16_t)AlarmId::PoolWaterLevelLow,
    (uint16_t)AlarmId::PoolWaterLevelLow,
};

constexpr bool kAlarmDefaultEnabled[TFTModuleS3::AlarmDashboardSlotCount] = {
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    false,
};

constexpr const char* kAlarmDefaultLabels[TFTModuleS3::AlarmDashboardSlotCount] = {
    "PSI bas",
    "PSI haut",
    "pH vide",
    "Chlore vide",
    "pH uptime",
    "ORP uptime",
    "Eau basse",
    "",
};

constexpr uint8_t kAlarmDefaultColorIds[TFTModuleS3::AlarmDashboardSlotCount] = {
    17U,
    17U,
    8U,
    10U,
    9U,
    7U,
    5U,
    19U,
};

const MqttConfigRouteProducer::Route kCfgRoutes[] = {
    {1, {(uint8_t)ConfigModuleId::TftS3, kCfgBranch}, "tft/s3", "tft/s3", (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgSensorBase + 0U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchSensorBase + 0U)}, kSensorModuleNames[0], kSensorModuleNames[0], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgSensorBase + 1U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchSensorBase + 1U)}, kSensorModuleNames[1], kSensorModuleNames[1], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgSensorBase + 2U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchSensorBase + 2U)}, kSensorModuleNames[2], kSensorModuleNames[2], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgSensorBase + 3U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchSensorBase + 3U)}, kSensorModuleNames[3], kSensorModuleNames[3], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgSensorBase + 4U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchSensorBase + 4U)}, kSensorModuleNames[4], kSensorModuleNames[4], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgSensorBase + 5U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchSensorBase + 5U)}, kSensorModuleNames[5], kSensorModuleNames[5], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgSensorBase + 6U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchSensorBase + 6U)}, kSensorModuleNames[6], kSensorModuleNames[6], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgSensorBase + 7U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchSensorBase + 7U)}, kSensorModuleNames[7], kSensorModuleNames[7], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgSensorBase + 8U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchSensorBase + 8U)}, kSensorModuleNames[8], kSensorModuleNames[8], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgSensorBase + 9U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchSensorBase + 9U)}, kSensorModuleNames[9], kSensorModuleNames[9], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgAlarmBase + 0U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchAlarmBase + 0U)}, kAlarmModuleNames[0], kAlarmModuleNames[0], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgAlarmBase + 1U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchAlarmBase + 1U)}, kAlarmModuleNames[1], kAlarmModuleNames[1], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgAlarmBase + 2U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchAlarmBase + 2U)}, kAlarmModuleNames[2], kAlarmModuleNames[2], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgAlarmBase + 3U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchAlarmBase + 3U)}, kAlarmModuleNames[3], kAlarmModuleNames[3], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgAlarmBase + 4U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchAlarmBase + 4U)}, kAlarmModuleNames[4], kAlarmModuleNames[4], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgAlarmBase + 5U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchAlarmBase + 5U)}, kAlarmModuleNames[5], kAlarmModuleNames[5], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgAlarmBase + 6U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchAlarmBase + 6U)}, kAlarmModuleNames[6], kAlarmModuleNames[6], (uint8_t)MqttPublishPriority::Normal, nullptr},
    {(uint16_t)(kCfgMsgAlarmBase + 7U), {(uint8_t)ConfigModuleId::TftS3, (uint8_t)(kCfgBranchAlarmBase + 7U)}, kAlarmModuleNames[7], kAlarmModuleNames[7], (uint8_t)MqttPublishPriority::Normal, nullptr},
};

static_assert((sizeof(kCfgRoutes) / sizeof(kCfgRoutes[0])) <= MqttConfigRouteProducer::MaxRoutes,
              "TFT S3 cfg routes exceed MQTT producer capacity");

const LocalUiBoardSpec& fallbackBoardSpec_()
{
    static constexpr LocalUiBoardSpec kFallback{
        {
            240,
            320,
            1,
            0,
            0,
            21,
            45,
            1,
            47,
            -1,
            2,
            48,
            false,
            true,
            40000000U,
            80
        },
        {
            11,
            120,
            true,
            -1,
            40,
            false,
            5000
        },
        {
            -1,
            -1,
            -1,
            115200U
        }
    };
    return kFallback;
}

int clampi_(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

uint8_t pctFromRssi_(int32_t dbm)
{
    const int v = clampi_((int)dbm, -100, -40);
    return (uint8_t)(((v + 100) * 100) / 60);
}

uint8_t barsFromPct_(uint8_t pct)
{
    return (uint8_t)((pct + 19U) / 20U);
}

struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
};

Rect overviewTopCardRect_(int16_t w, uint8_t column)
{
    // Native 320x240: x=1..158 and x=161..318, 2px between columns.
    const int16_t cardW = (int16_t)((w - 2 * kCardEdge - kCardGap) / 2);
    return Rect{
        (int16_t)(kCardEdge + (column > 0U ? cardW + kCardGap : 0)),
        (int16_t)(kHeaderH + 1),
        cardW,
        41
    };
}

Rect overviewStatusCardRect_(int16_t w, int16_t h, uint8_t row)
{
    // Cards: 76..128, 131..183, 186..238 on a 320x240 panel.
    // Header 32px, 41px IP/URL tiles, 2px inter-row gap, 1px bottom.
    const Rect topCard = overviewTopCardRect_(w, 0U);
    const int16_t firstY = (int16_t)(topCard.y + topCard.h + kCardGap);
    const int16_t cardH = (int16_t)((h - firstY - kCardEdge -
                                      (kOverviewSelectorRows - 1) * kOverviewSelectorGap) /
                                      kOverviewSelectorRows);
    return Rect{
        kCardEdge,
        (int16_t)(firstY + row * (cardH + kOverviewSelectorGap)),
        (int16_t)(w - 2 * kCardEdge),
        cardH
    };
}

Rect metricCardRect_(int16_t w, int16_t h, uint8_t index, bool dense = false)
{
    // Page 2: 5x2 (y=35,76,117,158,199). Page 3: 4x2
    // (y=33,85,137,189). Remainder pixels go to last row.
    const int16_t rows = dense ? 5 : 4;
    const int16_t firstY = (int16_t)(kHeaderH + (dense ? 3 : 1));
    const int16_t cardW = (int16_t)((w - 2 * kCardEdge - kCardGap) / 2);
    const int16_t available = (int16_t)(h - firstY - kCardEdge -
                                        (rows - 1) * kCardGap);
    const int16_t cardH = (int16_t)(available / rows);
    const int16_t row = (int16_t)(index / 2U);
    const int16_t col = (int16_t)(index % 2U);
    return Rect{
        (int16_t)(kCardEdge + col * (cardW + kCardGap)),
        (int16_t)(firstY + row * (cardH + kCardGap)),
        cardW,
        (int16_t)(cardH + (row == rows - 1 ? available % rows : 0))
    };
}

bool isDegreeCUnit_(const char* unit)
{
    if (!unit || unit[0] == '\0') return false;
    if ((uint8_t)unit[0] == 0xB0 && unit[1] == 'C' && unit[2] == '\0') return true;
    return (uint8_t)unit[0] == 0xC2 && (uint8_t)unit[1] == 0xB0 && unit[2] == 'C' && unit[3] == '\0';
}

// Icons are inferred from the live unit/alarm ID, not hardcoded to a TFT
// slot: the ten metrics and eight alarm slots remain user-configurable.
uint8_t metricGlyphKind_(const char* label, const char* unit)
{
    // Prioritise named measuring devices before the generic unit glyph.
    // These are only icon hints; they never change data binding or units.
    if (label && strncmp(label, "BME", 3) == 0) return 17U; // Sensor chip
    if (label && strncmp(label, "Volum", 5) == 0) return 18U; // Tank/volume
    if (label && (strncmp(label, "pH", 2) == 0 ||
                  strncmp(label, "PH", 2) == 0)) return 5U;
    if (isDegreeCUnit_(unit)) {
        if (label && (strncmp(label, "Air", 3) == 0)) return 14U;
        return 1U;
    }
    if (unit) {
        if (strcmp(unit, "mV") == 0) return 6U;
        if (strcmp(unit, "V") == 0) return 19U; // Voltage indicator
        if (strcmp(unit, "rpm") == 0) return 7U;
        if (strcmp(unit, "L") == 0) return 4U;
        if (strstr(unit, "/min") || strstr(unit, "/h")) return 2U;
        if (strcmp(unit, "hPa") == 0) return 13U;
        if (strcmp(unit, "PSI") == 0 || strcmp(unit, "bar") == 0) return 3U;
        if (strcmp(unit, "%") == 0) return 8U;
    }
    return 10U; // Unspecified metric: neutral information symbol.
}

uint8_t alarmGlyphKind_(uint16_t alarmId)
{
    switch ((AlarmId)alarmId) {
        case AlarmId::PoolPsiLow: return 11U;
        case AlarmId::PoolPsiHigh: return 12U;
        case AlarmId::PoolPhTankLow: return 5U;   // Flask
        case AlarmId::PoolChlorineTankLow: return 16U; // Canister
        case AlarmId::PoolPhPumpMaxUptime: return 9U; // Clock
        case AlarmId::PoolChlorinePumpMaxUptime: return 6U; // ORP lightning
        case AlarmId::PoolWaterLevelLow: return 15U; // Water level waves
        default: return 10U;
    }
}

void trimTrailingZeros_(char* text)
{
    if (!text) return;
    char* dot = strchr(text, '.');
    if (!dot) return;
    char* end = text + strlen(text);
    while (end > dot && end[-1] == '0') --end;
    if (end > dot && end[-1] == '.') --end;
    *end = '\0';
    if (strcmp(text, "-0") == 0) snprintf(text, 4, "0");
}

uint16_t alarmIndicatorColor_(uint32_t activeMask, uint32_t conditionMask, uint8_t alarmIndex)
{
    const uint32_t mask = (alarmIndex < 32U) ? (1UL << alarmIndex) : 0U;
    const bool active = (activeMask & mask) != 0U;
    const bool conditionTrue = (conditionMask & mask) != 0U;
    if (active && conditionTrue) return kColorAlarmAct;
    if (active) return kColorAlarmAck;
    return kColorGaugeOk;
}

uint16_t alarmTextColor_(uint32_t activeMask, uint8_t alarmIndex)
{
    const uint32_t mask = (alarmIndex < 32U) ? (1UL << alarmIndex) : 0U;
    return (activeMask & mask) != 0U ? kColorText : kColorMuted;
}
} // namespace

TFTModuleS3::TFTModuleS3(const BoardSpec& board)
    : displayCfg_(displaySpecFromBoard_(board)),
      display_(&spiBus_, displayCfg_.csPin, displayCfg_.dcPin, displayCfg_.rstPin)
{
    lastMotionMs_ = millis();
}

bool TFTModuleS3::allocateUiStorage_()
{
    if (uiStorage_) return true;
    // Verify PSRAM allocation benefit once at startup, without hot-path logs.
    const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psramBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    void* memory = heap_caps_malloc(sizeof(UiStorage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        LOGE("TFT unavailable: UI PSRAM allocation failed bytes=%u", (unsigned)sizeof(UiStorage));
        return false;
    }
    uiStorage_ = new (memory) UiStorage{};

    for (uint8_t i = 0; i < DashboardSlotCount; ++i) {
        DashboardSlotConfig& slotCfg = uiStorage_->dashboardCfg[i];
        slotCfg.enabled = (i < 8U); // Existing configuration untouched; new slots opt-in.
        slotCfg.runtimeUiId = kDashboardDefaultRuntimeUiIds[i];
        slotCfg.colorId = kDashboardDefaultColorIds[i];
        snprintf(slotCfg.label, sizeof(slotCfg.label), "%s", kDashboardDefaultLabels[i]);

        uiStorage_->dashboardEnabledVars[i].nvsKey = kSensorEnabledKeys[i];
        uiStorage_->dashboardEnabledVars[i].jsonName = "enabled";
        uiStorage_->dashboardEnabledVars[i].moduleName = kSensorModuleNames[i];
        uiStorage_->dashboardEnabledVars[i].type = ConfigType::Bool;
        uiStorage_->dashboardEnabledVars[i].value = &slotCfg.enabled;
        uiStorage_->dashboardEnabledVars[i].persistence = ConfigPersistence::Persistent;
        uiStorage_->dashboardEnabledVars[i].size = 0U;

        uiStorage_->dashboardRuntimeIdVars[i].nvsKey = kSensorRuntimeIdKeys[i];
        uiStorage_->dashboardRuntimeIdVars[i].jsonName = "runtime_ui_id";
        uiStorage_->dashboardRuntimeIdVars[i].moduleName = kSensorModuleNames[i];
        uiStorage_->dashboardRuntimeIdVars[i].type = ConfigType::UInt16;
        uiStorage_->dashboardRuntimeIdVars[i].value = &slotCfg.runtimeUiId;
        uiStorage_->dashboardRuntimeIdVars[i].persistence = ConfigPersistence::Persistent;
        uiStorage_->dashboardRuntimeIdVars[i].size = 0U;

        uiStorage_->dashboardLabelVars[i].nvsKey = kSensorLabelKeys[i];
        uiStorage_->dashboardLabelVars[i].jsonName = "label";
        uiStorage_->dashboardLabelVars[i].moduleName = kSensorModuleNames[i];
        uiStorage_->dashboardLabelVars[i].type = ConfigType::CharArray;
        uiStorage_->dashboardLabelVars[i].value = slotCfg.label;
        uiStorage_->dashboardLabelVars[i].persistence = ConfigPersistence::Persistent;
        uiStorage_->dashboardLabelVars[i].size = sizeof(slotCfg.label);

        uiStorage_->dashboardColorIdVars[i].nvsKey = kSensorColorIdKeys[i];
        uiStorage_->dashboardColorIdVars[i].jsonName = "color_id";
        uiStorage_->dashboardColorIdVars[i].moduleName = kSensorModuleNames[i];
        uiStorage_->dashboardColorIdVars[i].type = ConfigType::UInt8;
        uiStorage_->dashboardColorIdVars[i].value = &slotCfg.colorId;
        uiStorage_->dashboardColorIdVars[i].persistence = ConfigPersistence::Persistent;
        uiStorage_->dashboardColorIdVars[i].size = 0U;
    }

    for (uint8_t i = 0; i < AlarmDashboardSlotCount; ++i) {
        AlarmSlotConfig& slotCfg = uiStorage_->alarmDashboardCfg[i];
        slotCfg.enabled = kAlarmDefaultEnabled[i];
        slotCfg.alarmId = kAlarmDefaultIds[i];
        slotCfg.colorId = kAlarmDefaultColorIds[i];
        snprintf(slotCfg.label, sizeof(slotCfg.label), "%s", kAlarmDefaultLabels[i]);

        uiStorage_->alarmEnabledVars[i].nvsKey = kAlarmEnabledKeys[i];
        uiStorage_->alarmEnabledVars[i].jsonName = "enabled";
        uiStorage_->alarmEnabledVars[i].moduleName = kAlarmModuleNames[i];
        uiStorage_->alarmEnabledVars[i].type = ConfigType::Bool;
        uiStorage_->alarmEnabledVars[i].value = &slotCfg.enabled;
        uiStorage_->alarmEnabledVars[i].persistence = ConfigPersistence::Persistent;
        uiStorage_->alarmEnabledVars[i].size = 0U;

        uiStorage_->alarmIdVars[i].nvsKey = kAlarmIdKeys[i];
        uiStorage_->alarmIdVars[i].jsonName = "alarm_id";
        uiStorage_->alarmIdVars[i].moduleName = kAlarmModuleNames[i];
        uiStorage_->alarmIdVars[i].type = ConfigType::UInt16;
        uiStorage_->alarmIdVars[i].value = &slotCfg.alarmId;
        uiStorage_->alarmIdVars[i].persistence = ConfigPersistence::Persistent;
        uiStorage_->alarmIdVars[i].size = 0U;

        uiStorage_->alarmLabelVars[i].nvsKey = kAlarmLabelKeys[i];
        uiStorage_->alarmLabelVars[i].jsonName = "label";
        uiStorage_->alarmLabelVars[i].moduleName = kAlarmModuleNames[i];
        uiStorage_->alarmLabelVars[i].type = ConfigType::CharArray;
        uiStorage_->alarmLabelVars[i].value = slotCfg.label;
        uiStorage_->alarmLabelVars[i].persistence = ConfigPersistence::Persistent;
        uiStorage_->alarmLabelVars[i].size = sizeof(slotCfg.label);

        uiStorage_->alarmColorIdVars[i].nvsKey = kAlarmColorIdKeys[i];
        uiStorage_->alarmColorIdVars[i].jsonName = "color_id";
        uiStorage_->alarmColorIdVars[i].moduleName = kAlarmModuleNames[i];
        uiStorage_->alarmColorIdVars[i].type = ConfigType::UInt8;
        uiStorage_->alarmColorIdVars[i].value = &slotCfg.colorId;
        uiStorage_->alarmColorIdVars[i].persistence = ConfigPersistence::Persistent;
        uiStorage_->alarmColorIdVars[i].size = 0U;
    }
    const size_t internalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psramAfter = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    LOGI("TFT UI PSRAM bytes=%u int_delta=%ld psram_delta=%ld int_largest=%u",
         (unsigned)sizeof(UiStorage),
         (long)((int64_t)internalBefore - (int64_t)internalAfter),
         (long)((int64_t)psramBefore - (int64_t)psramAfter),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
}

St7789DisplaySpec TFTModuleS3::displaySpecFromBoard_(const BoardSpec& board)
{
    const LocalUiBoardSpec* sup = boardLocalUiConfig(board);
    return sup ? sup->display : fallbackBoardSpec_().display;
}

void TFTModuleS3::init(ConfigStore& cfg, ServiceRegistry& services)
{
    if (!allocateUiStorage_()) return;

    cfgStore_ = &cfg;
    serviceRegistry_ = &services;
    constexpr uint8_t module = (uint8_t)ConfigModuleId::TftS3;
    cfg.registerVar(enabledVar_, module, kCfgBranch);
    cfg.registerVar(autoOffVar_, module, kCfgBranch);
    cfg.registerVar(motionIoIdVar_, module, kCfgBranch);
    cfg.registerVar(flipVar_, module, kCfgBranch);

    for (uint8_t i = 0; i < DashboardSlotCount; ++i) {
        const uint8_t branch = (uint8_t)(kCfgBranchSensorBase + i);
        cfg.registerVar(uiStorage_->dashboardEnabledVars[i], module, branch);
        cfg.registerVar(uiStorage_->dashboardRuntimeIdVars[i], module, branch);
        cfg.registerVar(uiStorage_->dashboardLabelVars[i], module, branch);
        cfg.registerVar(uiStorage_->dashboardColorIdVars[i], module, branch);
    }

    for (uint8_t i = 0; i < AlarmDashboardSlotCount; ++i) {
        const uint8_t branch = (uint8_t)(kCfgBranchAlarmBase + i);
        cfg.registerVar(uiStorage_->alarmEnabledVars[i], module, branch);
        cfg.registerVar(uiStorage_->alarmIdVars[i], module, branch);
        cfg.registerVar(uiStorage_->alarmLabelVars[i], module, branch);
        cfg.registerVar(uiStorage_->alarmColorIdVars[i], module, branch);
    }

    alarmSvc_ = services.get<AlarmService>(ServiceId::Alarm);
    ioSvc_ = services.get<IOServiceV2>(ServiceId::Io);
    dsSvc_ = services.get<DataStoreService>(ServiceId::DataStore);
    const EventBusService* ebSvc = services.get<EventBusService>(ServiceId::EventBus);
    eventBus_ = ebSvc ? ebSvc->bus : nullptr;

    if (eventBus_) {
        eventBus_->subscribe(EventId::ConfigChanged, &TFTModuleS3::onEventStatic_, this);
        eventBus_->subscribe(EventId::DataChanged, &TFTModuleS3::onEventStatic_, this);
    }
}

void TFTModuleS3::onConfigLoaded(ConfigStore&, ServiceRegistry& services)
{
    poolConfigurationSvc_ = services.get<PoolConfigurationService>(ServiceId::PoolConfiguration);
    firmwareUpdateSvc_ = services.get<FirmwareUpdateService>(ServiceId::FirmwareUpdate);
    localeSvc_ = services.get<LocaleService>(ServiceId::Locale);
    if (!uiStorage_) return;

    if (!ioSvc_) ioSvc_ = services.get<IOServiceV2>(ServiceId::Io);
    if (!cfgMqttPubConfigured_) {
        uiStorage_->cfgMqttPub.configure(this,
                              kCfgProducerId,
                              kCfgRoutes,
                              (uint8_t)(sizeof(kCfgRoutes) / sizeof(kCfgRoutes[0])),
                              services);
        cfgMqttPubConfigured_ = true;
    }
    resetMotionInput_();
    redrawRequested_ = true;
}

void TFTModuleS3::loop()
{
    if (!uiStorage_) {
        delayNextCycle(pdMS_TO_TICKS(500));
        return;
    }

    if (!cfgData_.enabled) {
        applyBacklight_(false);
        delayNextCycle(pdMS_TO_TICKS(250));
        return;
    }

    if (!beginDisplay_()) {
        delayNextCycle(pdMS_TO_TICKS(500));
        return;
    }

    applyOrientation_();
    const uint32_t now = millis();
    FirmwareUpdateDisplaySnapshot update{};
    bool updateVisible = readUpdateSnapshot_(update);
    if (updateVisible) {
        // Persistent terminal states must not re-enter the update screen
        // every 20 seconds after the user returns to the dashboard.
        if (update.operationId == updateDismissedOperation_) {
            updateVisible = false;
        } else {
            const bool terminal = update.state == FirmwareUpdateDisplayState::Error ||
                                  update.state == FirmwareUpdateDisplayState::Done;
            if (terminal) {
                if (updateTerminalAtMs_ == 0U ||
                    update.operationId != updateLastOperation_) updateTerminalAtMs_ = now;
                if (uint32_t(now - updateTerminalAtMs_) >= kUpdateResultHoldMs) {
                    updateDismissedOperation_ = update.operationId;
                    updateVisible = false;
                }
            } else {
                updateTerminalAtMs_ = 0U;
            }
        }
    }
    if (updateVisible) {
        // Override the PIR timeout during a critical update: always show the
        // state even when the TFT was previously in power-saving mode.
        applyBacklight_(true);
        const bool newOperation = !updateScreenActive_ ||
                                  update.operationId != updateLastOperation_ ||
                                  update.target != updateLastTarget_;
        if (newOperation) {
            updateLastWasZip_ = false;
            updateCompletedStage_[0] = '\0';
            updateLastStage_[0] = '\0';
            updateLastPercent_ = 255U;
        }
        if (strncmp(update.stage, "zip_", 4U) == 0) {
            updateLastWasZip_ = true;
        }
        const bool english = localeSvc_ && localeSvc_->language &&
            tft_update::isEnglish(localeSvc_->language(localeSvc_->ctx));
        const bool changedStage = strcmp(updateLastStage_, update.stage) != 0;
        if (!newOperation && changedStage && updateLastStage_[0] &&
            updateLastState_ != FirmwareUpdateDisplayState::Error &&
            update.state != FirmwareUpdateDisplayState::Error) {
            // Only mark a step complete if it was ACTUALLY observed.
            // Some OTA paths skip PSRAM verification or use direct streaming.
            snprintf(updateCompletedStage_, sizeof(updateCompletedStage_), "%s",
                     updateLastStage_);
        }
        const bool stateChanged = update.state != updateLastState_;
        const bool targetChanged = update.target != updateLastTarget_;
        const bool languageChanged = english != updateLastEnglish_;
        uint8_t phasePercent = update.stage[0] ? update.stageProgress : update.progress;
        if (update.state == FirmwareUpdateDisplayState::Done ||
            update.state == FirmwareUpdateDisplayState::Rebooting) {
            phasePercent = 100U;
        } else if (update.state == FirmwareUpdateDisplayState::Error &&
                   !newOperation && updateLastPercent_ <= 100U) {
            phasePercent = updateLastPercent_;
        }
        if (phasePercent > 100U) phasePercent = 100U;
        const tft_update::RenderDirty dirty = tft_update::renderDirty(
            newOperation, changedStage, stateChanged, targetChanged,
            languageChanged, phasePercent, updateLastPercent_);
        const bool changed = dirty.title || dirty.progress || dirty.details;
        const bool urgent = newOperation || changedStage || stateChanged ||
                            targetChanged || languageChanged;
        if (changed && (urgent || uint32_t(now - updateLastRenderMs_) >= kUpdateRenderMinMs)) {
            renderUpdate_(update, english, phasePercent, dirty);
            updateLastRenderMs_ = now;
            updateLastPercent_ = phasePercent;
            updateLastState_ = update.state;
            updateLastTarget_ = update.target;
            updateLastOperation_ = update.operationId;
            updateLastEnglish_ = english;
            snprintf(updateLastStage_, sizeof(updateLastStage_), "%s", update.stage);
        }
        updateScreenActive_ = true;
        delayNextCycle(pdMS_TO_TICKS(80));
        return;
    }
    if (updateScreenActive_) {
        updateScreenActive_ = false;
        updateTerminalAtMs_ = 0U;
        splashHeld_ = true;
        pageCycleStartMs_ = now;
        lastRenderedPageCycle_ = 0xFFFFFFFFU;
        invalidateRenderCache_();
        redrawRequested_ = true;
    }
    updateBacklight_();
    if (!splashHeld_) {
        if ((int32_t)(now - splashHoldUntilMs_) < 0) {
            delayNextCycle(pdMS_TO_TICKS(30));
            return;
        }
        splashHeld_ = true;
        pageCycleStartMs_ = now;
        lastRenderedPageCycle_ = 0xFFFFFFFFU;
        redrawRequested_ = true;
    }

    const uint32_t pageCycle = currentPageCycle_();
    const bool pageChanged = pageCycle != lastRenderedPageCycle_;
    const bool fullRedraw = !layoutDrawn_ || pageChanged;
    if (fullRedraw || redrawRequested_ || (now - lastRenderMs_) >= kRenderPeriodMs) {
        render_(fullRedraw);
        lastRenderedPageCycle_ = pageCycle;
    }
    delayNextCycle(pdMS_TO_TICKS(30));
}

void TFTModuleS3::onEventStatic_(const Event& e, void* user)
{
    TFTModuleS3* self = static_cast<TFTModuleS3*>(user);
    if (self) self->onEvent_(e);
}

void TFTModuleS3::onEvent_(const Event& e)
{
    if (!uiStorage_) return;

    if (e.id == EventId::ConfigChanged && e.payload && e.len >= sizeof(ConfigChangedPayload)) {
        const ConfigChangedPayload* p = static_cast<const ConfigChangedPayload*>(e.payload);
        if (p->moduleId == (uint8_t)ConfigModuleId::TftS3) {
            resetMotionInput_();
            invalidateRenderCache_();
            redrawRequested_ = true;
        }
        return;
    }
    if (e.id == EventId::DataChanged) {
        redrawRequested_ = true;
    }
}

bool TFTModuleS3::beginDisplay_()
{
    if (displayReady_) return true;
    if (displayCfg_.csPin < 0 || displayCfg_.dcPin < 0 || displayCfg_.mosiPin < 0 || displayCfg_.sclkPin < 0) {
        LOGW("TFT disabled: invalid SPI pins");
        return false;
    }

    spiBus_.begin(displayCfg_.sclkPin, displayCfg_.misoPin, displayCfg_.mosiPin, displayCfg_.csPin);
    if (displayCfg_.backlightPin >= 0) {
        pinMode(displayCfg_.backlightPin, OUTPUT);
        digitalWrite(displayCfg_.backlightPin, HIGH);
        backlightOn_ = true;
    }
    display_.setSPISpeed(displayCfg_.spiHz);
    display_.init(displayCfg_.resX, displayCfg_.resY);
    display_.setColRowStart(displayCfg_.colStart, displayCfg_.rowStart);
    applyOrientation_();
    display_.invertDisplay(displayCfg_.invertColors);
    display_.setTextWrap(false);
    display_.fillScreen(color_(kColorBg));
    displayReady_ = true;
    startSplash_(kStartupSplashHoldMs);
    LOGI("TFT S3 ready %ux%u", (unsigned)displayCfg_.resX, (unsigned)displayCfg_.resY);
    return true;
}

void TFTModuleS3::applyOrientation_()
{
    const uint8_t rotation = (displayCfg_.rotation + (cfgData_.flip ? 2U : 0U)) & 0x03U;
    if (displayReady_ && display_.getRotation() == rotation) return;

    display_.setRotation(rotation);
    invalidateRenderCache_();
    redrawRequested_ = true;
    if (displayReady_ && !splashHeld_) drawBootLogo_();
}

void TFTModuleS3::applyBacklight_(bool on)
{
    if (backlightOn_ == on) return;
    if (displayCfg_.backlightPin >= 0) {
        pinMode(displayCfg_.backlightPin, OUTPUT);
        digitalWrite(displayCfg_.backlightPin, on ? HIGH : LOW);
    }
    backlightOn_ = on;
    if (on) {
        invalidateRenderCache_();
        redrawRequested_ = true;
    }
}

void TFTModuleS3::resetMotionInput_()
{
    motionInputReady_ = false;
    motionReadErrorLogged_ = false;
    lastMotionMs_ = millis();
    if (cfgData_.enabled && displayReady_) applyBacklight_(true);
}

void TFTModuleS3::updateBacklight_()
{
    if (!cfgData_.autoOff60s) {
        applyBacklight_(true);
        return;
    }

    const uint32_t now = millis();
    uint8_t motionActive = 0U;
    IoStatus status = IO_ERR_INVALID_ARG;
    if (cfgData_.motionIoId >= IO_ID_DI_BASE && cfgData_.motionIoId < IO_ID_AI_BASE) {
        status = (ioSvc_ && ioSvc_->readDigital)
            ? ioSvc_->readDigital(ioSvc_->ctx, cfgData_.motionIoId, &motionActive, nullptr, nullptr)
            : IO_ERR_NOT_READY;
    }
    if (status != IO_OK) {
        if (!motionReadErrorLogged_) {
            LOGW("TFT motion input unavailable io_id=%u status=%u; backlight forced on",
                 (unsigned)cfgData_.motionIoId,
                 (unsigned)status);
            motionReadErrorLogged_ = true;
        }
        motionInputReady_ = false;
        lastMotionMs_ = now;
        applyBacklight_(true);
        return;
    }

    if (!motionInputReady_) lastMotionMs_ = now;
    motionInputReady_ = true;
    motionReadErrorLogged_ = false;
    if (motionActive != 0U) lastMotionMs_ = now;
    if (motionActive != 0U && !backlightOn_) {
        // Prepare the logo while the backlight is off to avoid flashing the old page.
        startSplash_(kWakeSplashHoldMs);
    }
    applyBacklight_((now - lastMotionMs_) < kBacklightTimeoutMs);
}

void TFTModuleS3::render_(bool force)
{
    if (!displayReady_) return;
    if (!backlightOn_ && !force) return;

    const uint32_t now = millis();
    if (!force && (now - lastRenderMs_) < displayCfg_.minRenderGapMs) return;

    const uint32_t pageCycle = currentPageCycle_() % 3U;
    const Page page = pageCycle == 0U ? Page::Overview : (pageCycle == 1U ? Page::Metrics : Page::Alarms);
    const uint8_t pageId = (uint8_t)page;
    // A change in enabled sensors changes their screen positions. Force
    // one complete repaint so old cards cannot remain in vacated cells.
    uint16_t enabledMask = 0U;
    if (page == Page::Metrics) {
        for (uint8_t i = 0U; i < DashboardSlotCount; ++i)
            if (uiStorage_->dashboardCfg[i].enabled)
                enabledMask |= (uint16_t)(1U << i);
    }
    const bool denseMetrics = true; // Always preserve the approved 5x2 screen-2 grid.
    const bool sensorLayoutChanged = page == Page::Metrics &&
                                     enabledMask != lastSensorEnabledMask_;
    const bool fullRedraw = force || !layoutDrawn_ || lastPage_ != pageId ||
                            sensorLayoutChanged;
    if (page == Page::Metrics) lastSensorEnabledMask_ = enabledMask;
    if (fullRedraw) {
        drawStaticLayout_(page);
        overviewCache_.valid = false;
        for (uint8_t i = 0U; i < DashboardSlotCount; ++i) {
            uiStorage_->dashboardSlotCache[i].valid = false;
        }
        for (uint8_t i = 0U; i < AlarmDashboardSlotCount; ++i) {
            uiStorage_->alarmSlotCache[i].valid = false;
        }
    }

    // Identical live header on pages 1, 2 and 3, refreshed only when needed.
    char timeBuf[16] = "--:--";
    time_t t = time(nullptr);
    if (t > 1600000000) {
        struct tm tmv{};
        localtime_r(&t, &tmv);
        (void)strftime(timeBuf, sizeof(timeBuf), "%H:%M", &tmv);
    }
    const bool apMode = (WiFi.getMode() & WIFI_MODE_AP) != 0 && !WiFi.isConnected();
    const uint8_t bars = WiFi.isConnected() ? barsFromPct_(pctFromRssi_(WiFi.RSSI())) : 0U;
    const bool headerChanged = fullRedraw || !overviewCache_.valid ||
                               overviewCache_.apMode != apMode ||
                               overviewCache_.wifiBars != bars ||
                               strncmp(overviewCache_.timeText, timeBuf,
                                       sizeof(overviewCache_.timeText)) != 0;
    if (headerChanged) drawHeader_(timeBuf, bars, apMode, (uint8_t)pageCycle,
                                   fullRedraw || !overviewCache_.valid);
    snprintf(overviewCache_.timeText, sizeof(overviewCache_.timeText), "%s", timeBuf);
    overviewCache_.apMode = apMode;
    overviewCache_.wifiBars = bars;

    if (page == Page::Overview) {
        char ip[20] = {0};
        formatIp_(ip, sizeof(ip));
        OverviewStatusRenderState status{};
        readOverviewStatus_(status);

        const int16_t w = display_.width();
        const Rect leftTop = overviewTopCardRect_(w, 0U);
        const Rect rightTop = overviewTopCardRect_(w, 1U);
        if (fullRedraw || !overviewCache_.valid || strncmp(overviewCache_.ip, ip, sizeof(overviewCache_.ip)) != 0) {
            drawCenteredTextCard_(leftTop.x, leftTop.y, leftTop.w, leftTop.h, ip, false);
        }
        if (fullRedraw || !overviewCache_.valid) {
            drawCenteredTextCard_(rightTop.x, rightTop.y, rightTop.w, rightTop.h, "http://flowio.local", false);
        }
        if (fullRedraw || !overviewCache_.valid || !overviewStatusEquals_(overviewCache_.status, status)) {
            drawOverviewSelectors_(status);
        }

        snprintf(overviewCache_.ip, sizeof(overviewCache_.ip), "%s", ip);
        overviewCache_.status = status;
    } else if (page == Page::Metrics) {
        const int16_t w = display_.width();
        const int16_t h = display_.height();
        uint8_t visibleIndex = 0U;
        for (uint8_t i = 0U; i < DashboardSlotCount; ++i) {
            if (!uiStorage_->dashboardCfg[i].enabled) continue;
            DashboardSlotRenderState state{};
            readDashboardSlotState_(i, state);
            const DashboardSlotRenderState& previous = uiStorage_->dashboardSlotCache[i];
            const RuntimeUiId runtimeId = uiStorage_->dashboardCfg[i].runtimeUiId;
            const ValueId dataId = runtimeDataValueId(runtimeId);
            // Mode 2 publishes iXX.flow with a volumetric-flow unit. Only those
            // TFT cards are rate-limited; regular values and counters stay live.
            const bool flowId = dataId >= ValueIds::ConvertedRate &&
                                dataId < ValueIds::Derived;
            const bool flowUnit = strcmp(state.unit, "l/min") == 0 ||
                                  strcmp(state.unit, "l/h") == 0 ||
                                  strcmp(state.unit, "m3/min") == 0 ||
                                  strcmp(state.unit, "m3/h") == 0;
            const bool flowCard = flowId && flowUnit;
            const bool sameFlowBinding = lastFlowRuntimeId_[i] == runtimeId;
            const bool valueOnlyChanged = previous.valid == state.valid &&
                                          previous.enabled == state.enabled &&
                                          previous.available == state.available &&
                                          previous.cardBg == state.cardBg &&
                                          strncmp(previous.label, state.label, sizeof(state.label)) == 0 &&
                                          strncmp(previous.unit, state.unit, sizeof(state.unit)) == 0;
            const bool flowThrottle = !fullRedraw && flowCard && sameFlowBinding &&
                                      valueOnlyChanged &&
                                      (uint32_t)(now - lastFlowRenderMs_[i]) < kFlowMeterTftRefreshMs;
            if (!flowThrottle &&
                (fullRedraw || !dashboardSlotStateEquals_(previous, state))) {
                const Rect r = metricCardRect_(w, h, visibleIndex, denseMetrics);
                // Update only text on a live value change. Leave the border,
                // icon and static separator untouched to prevent ST7789 flicker.
                drawDashboardSlot_(i, r.x, r.y, r.w, r.h, state, denseMetrics,
                                   !fullRedraw && valueOnlyChanged);
                uiStorage_->dashboardSlotCache[i] = state;
                if (flowCard) {
                    lastFlowRenderMs_[i] = now;
                    lastFlowRuntimeId_[i] = runtimeId;
                } else {
                    lastFlowRuntimeId_[i] = 0U;
                }
            }
            ++visibleIndex;
        }
    } else {
        const int16_t w = display_.width();
        const int16_t h = display_.height();
        for (uint8_t i = 0U; i < AlarmDashboardSlotCount; ++i) {
            AlarmSlotRenderState state{};
            readAlarmSlotState_(i, state);
            const AlarmSlotRenderState& previous = uiStorage_->alarmSlotCache[i];
            if (fullRedraw || !alarmSlotStateEquals_(previous, state)) {
                const Rect r = metricCardRect_(w, h, i);
                // Only the two 14px indicator circles need an update when
                // the alarm runtime changes but its card layout stays fixed.
                const bool indicatorsOnly = !fullRedraw && previous.valid &&
                    previous.enabled && state.enabled &&
                    previous.alarmId == state.alarmId &&
                    previous.cardBg == state.cardBg &&
                    strncmp(previous.label, state.label, sizeof(state.label)) == 0;
                if (indicatorsOnly) {
                    const int16_t stateX = (int16_t)(r.x + r.w - 13);
                    drawAlarmStateIcon_(stateX, (int16_t)(r.y + 13), "L",
                                        state.latched, state.available);
                    drawAlarmStateIcon_(stateX, (int16_t)(r.y + r.h - 13), "C",
                                        state.conditionTrue,
                                        state.available && state.conditionKnown);
                } else {
                    drawAlarmDashboardSlot_(i, r.x, r.y, r.w, r.h, state);
                }
                uiStorage_->alarmSlotCache[i] = state;
            }
        }
    }

    overviewCache_.valid = true;
    layoutDrawn_ = true;
    lastPage_ = pageId;
    lastRenderMs_ = now;
    redrawRequested_ = false;
}

void TFTModuleS3::invalidateRenderCache_()
{
    layoutDrawn_ = false;
    lastPage_ = 0xFFU;
    overviewCache_.valid = false;
    for (uint8_t i = 0U; i < DashboardSlotCount; ++i) {
        uiStorage_->dashboardSlotCache[i].valid = false;
    }
    for (uint8_t i = 0U; i < AlarmDashboardSlotCount; ++i) {
        uiStorage_->alarmSlotCache[i].valid = false;
    }
}

uint16_t TFTModuleS3::color_(uint16_t rgb565) const
{
    return displayCfg_.swapColorBytes ? (uint16_t)((rgb565 << 8) | (rgb565 >> 8)) : rgb565;
}

void TFTModuleS3::setGfxFont_(const GFXfont* font, uint16_t fg, uint16_t bg)
{
    display_.setFont(font);
    display_.setTextSize(1);
    display_.setTextColor(color_(fg), color_(bg));
}

int16_t TFTModuleS3::gfxTextWidth_(const char* text)
{
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    display_.getTextBounds(text ? text : "", 0, 0, &x1, &y1, &w, &h);
    return (int16_t)w;
}

int16_t TFTModuleS3::gfxBaselineCenteredInBox_(const char* text, int16_t boxY, int16_t boxH)
{
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    display_.getTextBounds(text ? text : "", 0, 0, &x1, &y1, &w, &h);
    return (int16_t)(boxY + ((boxH - (int16_t)h) / 2) - y1);
}

void TFTModuleS3::drawGfxText_(const GFXfont* font, uint16_t fg, uint16_t bg, int16_t x, int16_t baselineY, const char* text)
{
    setGfxFont_(font, fg, bg);
    display_.setCursor(x, baselineY);
    display_.print(text ? text : "");
}

void TFTModuleS3::drawGfxTextCenteredY_(const GFXfont* font,
                                        uint16_t fg,
                                        uint16_t bg,
                                        int16_t x,
                                        int16_t boxY,
                                        int16_t boxH,
                                        const char* text)
{
    setGfxFont_(font, fg, bg);
    display_.setCursor(x, gfxBaselineCenteredInBox_(text, boxY, boxH));
    display_.print(text ? text : "");
}

void TFTModuleS3::startSplash_(uint32_t holdMs)
{
    invalidateRenderCache_();
    redrawRequested_ = true;
    splashHeld_ = false;
    pageCycleStartMs_ = 0;
    lastRenderedPageCycle_ = 0xFFFFFFFFU;
    drawBootLogo_();
    splashHoldUntilMs_ = millis() + holdMs;
}

bool TFTModuleS3::readUpdateSnapshot_(FirmwareUpdateDisplaySnapshot& out)
{
    // Resolve lazily because TFT and firmware modules have independent startup
    // tasks. Never parse status JSON or lock an OTA worker from TFT code.
    if (!firmwareUpdateSvc_ && serviceRegistry_) {
        firmwareUpdateSvc_ = serviceRegistry_->get<FirmwareUpdateService>(ServiceId::FirmwareUpdate);
    }
    if (!localeSvc_ && serviceRegistry_) {
        localeSvc_ = serviceRegistry_->get<LocaleService>(ServiceId::Locale);
    }
    if (!firmwareUpdateSvc_ || !firmwareUpdateSvc_->displaySnapshot ||
        !firmwareUpdateSvc_->displaySnapshot(firmwareUpdateSvc_->ctx, &out) ||
        out.operationId == 0U) return false;
    switch (out.state) {
        case FirmwareUpdateDisplayState::Queued:
        case FirmwareUpdateDisplayState::Downloading:
        case FirmwareUpdateDisplayState::Flashing:
        case FirmwareUpdateDisplayState::Rebooting:
        case FirmwareUpdateDisplayState::Done:
        case FirmwareUpdateDisplayState::Error:
            return true;
        default: return false;
    }
}


void TFTModuleS3::drawUpdateBrandBitmap_(const uint16_t* bitmap,
                                         int16_t sourceW, int16_t sourceH,
                                         int16_t x, int16_t y, int16_t w, int16_t h,
                                         bool wordmark)
{
    // Original RGB565 assets are white-matted. Downsample and unmatte them
    // directly on the black TFT background; no allocation or extra assets.
    // The logo and wordmark are drawn once per update operation.
    display_.startWrite();
    for (int16_t row = 0; row < h; ++row) {
        const int32_t srcRow = int32_t(row) * sourceH / h;
        for (int16_t col = 0; col < w; ++col) {
            const int32_t srcCol = int32_t(col) * sourceW / w;
            const uint16_t pixel = pgm_read_word(bitmap + srcRow * sourceW + srcCol);
            if (pixel == 0xFFFFU) continue;
            const uint8_t r = uint8_t(((pixel >> 11) & 0x1FU) * 255U / 31U);
            const uint8_t g = uint8_t(((pixel >> 5) & 0x3FU) * 255U / 63U);
            const uint8_t b = uint8_t((pixel & 0x1FU) * 255U / 31U);
            uint16_t ink;
            if (wordmark) {
                const uint8_t luminance = uint8_t(
                    (uint32_t(r) * 54U + uint32_t(g) * 183U + uint32_t(b) * 19U) / 256U);
                const uint8_t brightness = uint8_t(255U - luminance);
                ink = rgb565_(brightness, brightness, brightness);
            } else {
                // Approximate inverse of the white anti-aliasing matte for
                // the blue pictogram: no residual bright rectangle.
                ink = rgb565_(0U, g > r ? uint8_t(g - r) : 0U,
                              b > r ? uint8_t(b - r) : 0U);
            }
            if (ink) display_.writePixel(x + col, y + row, color_(ink));
        }
    }
    display_.endWrite();
}

void TFTModuleS3::renderUpdate_(const FirmwareUpdateDisplaySnapshot& u,
                                bool english, uint8_t pct,
                                const tft_update::RenderDirty& dirty)
{
    const int16_t w = display_.width();
    const int16_t h = display_.height();
    const uint16_t dark = rgb565_(0, 0, 0);
    const uint16_t faded = rgb565_(143, 164, 183);
    const uint16_t cyan = rgb565_(0, 201, 233);
    const uint16_t green = rgb565_(51, 221, 124);
    const uint16_t red = rgb565_(255, 106, 113);
    const uint16_t white = rgb565_(241, 248, 255);
    const uint16_t track = rgb565_(26, 43, 61);
    const bool nextion = u.target == FirmwareUpdateTarget::Nextion;
    const bool zipUpdate = !nextion && updateLastWasZip_;
    const bool error = u.state == FirmwareUpdateDisplayState::Error;
    const bool done = u.state == FirmwareUpdateDisplayState::Done;
    const bool queued = u.state == FirmwareUpdateDisplayState::Queued;
    const bool rebooting = u.state == FirmwareUpdateDisplayState::Rebooting;
    const tft_update::Text stage = tft_update::steps(nextion, u.stage, english,
                                                    queued, rebooting, zipUpdate);
    constexpr int16_t bx = 12, by = 103, bh = 17;
    const int16_t bw = w - 24;

    if (dirty.full) {
        // Compact black brand (drawn once, never touched by progress ticks).
        display_.fillScreen(color_(dark));
        drawUpdateBrandBitmap_(kFlowIoLogoBitmap,
                               kFlowIoLogoWidth, kFlowIoLogoHeight,
                               (w - 96) / 2, 3, 96, 46, false);
        drawUpdateBrandBitmap_(kFlowIoWordmarkSplashBitmap,
                               kFlowIoWordmarkSplashWidth, kFlowIoWordmarkSplashHeight,
                               (w - 69) / 2, 48, 69, 19, true);

        // Stationary bevel + drop shadow: no animated glare or full redraw.
        display_.fillRoundRect(bx + 1, by + 3, bw, bh, 7,
                               color_(rgb565_(5, 12, 22)));
        display_.fillRoundRect(bx, by, bw, bh, 7,
                               color_(rgb565_(42, 66, 88)));
        display_.drawRoundRect(bx, by, bw, bh, 7,
                               color_(rgb565_(73, 105, 124)));
        display_.drawFastHLine(bx + 9, by + 1, bw - 18,
                               color_(rgb565_(89, 121, 145)));
        display_.drawFastHLine(bx + 9, by + bh - 2, bw - 18,
                               color_(rgb565_(13, 25, 42)));
    }

    if (dirty.title) {
        display_.fillRect(8, 69, w - 16, 29, color_(dark));
        const char* title = error ?
            (english ? "UPDATE FAILED" : "ECHEC DE LA MAJ") :
            done ? (english ? "UPDATE COMPLETE" : "MISE A JOUR OK") :
            nextion ? (english ? "NEXTION UPDATE" : "MISE A JOUR NEXTION") :
            zipUpdate ? "ZIP FIRMWARE + SPIFFS" :
                        (english ? "FIRMWARE + SPIFFS" : "MISE A JOUR OTA");
        setGfxFont_(&FreeSansBold9pt7b, error ? red : cyan, dark);
        const int16_t titleW = gfxTextWidth_(title);
        display_.setCursor((w - titleW) / 2, 91);
        display_.print(title);
        // Bolder 8pt warning next to the logo, within x=2..109.
        // The compact icon starts at x=112 and remains untouched.
        display_.fillRect(2, 3, 108, 38, color_(dark));
        const uint16_t warning = rgb565_(255, 206, 87);
        display_.drawCircle(9, 20, 5, color_(warning));
        display_.drawFastVLine(9, 17, 4, color_(warning));
        display_.drawPixel(9, 23, color_(warning));
        setGfxFont_(&FreeSansBold8pt7b, warning, dark);
        display_.setCursor(19, 18);
        display_.print(english ? "DO NOT" : "NE PAS");
        display_.setCursor(19, 35);
        display_.print(english ? "TURN OFF" : "ETEINDRE");
    }

    if (dirty.progress) {
        // Simple 3D gradient, refreshed only inside the original progress track.
        // No scrolling shine: preserves the anti-flicker partial redraw design.
        const int16_t x = bx + 3, y = by + 4;
        const int16_t interior = bw - 6;
        constexpr int16_t barH = 9;
        display_.fillRect(x, y, interior, barH, color_(track));
        display_.drawFastHLine(x, y, interior, color_(rgb565_(44, 67, 88)));

        // Keep a 4px breathing room at the rounded left end of the track.
        // Reduce the fill width by the same amount so 100% still ends at
        // the original right edge (and the outer bevel stays untouched).
        constexpr int16_t fillLeftInset = 4;
        const int16_t fillX = x + fillLeftInset;
        const int16_t fillMaxWidth = interior - fillLeftInset;
        const int16_t filled = int16_t(int32_t(fillMaxWidth) * pct / 100);
        if (filled > 0) {
            const uint16_t glow = error ? rgb565_(255, 176, 174) :
                                  done ? rgb565_(138, 255, 186) :
                                         rgb565_(116, 244, 255);
            const uint16_t upper = error ? rgb565_(255, 116, 123) :
                                   done ? rgb565_(54, 220, 132) :
                                          rgb565_(30, 215, 247);
            const uint16_t middle = error ? rgb565_(222, 62, 78) :
                                    done ? rgb565_(36, 163, 94) :
                                           rgb565_(0, 152, 211);
            const uint16_t lower = error ? rgb565_(107, 27, 51) :
                                   done ? rgb565_(11, 85, 56) :
                                          rgb565_(3, 64, 124);
            display_.fillRect(fillX, y, filled, 2, color_(glow));
            display_.fillRect(fillX, y + 2, filled, 3, color_(upper));
            display_.fillRect(fillX, y + 5, filled, 2, color_(middle));
            display_.fillRect(fillX, y + 7, filled, 2, color_(lower));
            display_.drawFastVLine(fillX + filled - 1, y + 1, barH - 2, color_(glow));
        }

        // Display the raw percentage only, without the confusing STEP/ETAPE.
        display_.fillRect(w - 76, 124, 64, 17, color_(dark));
        char pctText[8];
        snprintf(pctText, sizeof(pctText), "%u%%", unsigned(pct));
        display_.setFont(nullptr);
        display_.setTextSize(2);
        display_.setTextColor(color_(white), color_(dark));
        display_.setCursor(w - 12 - int16_t(strlen(pctText) * 12), 124);
        display_.print(pctText);
        display_.setTextSize(1);
    }

    if (dirty.details) {
        // Chronological view: finished step at TOP, active step in the
        // CENTER and next step at BOTTOM. No disconnected "OK" in between.
        // Three bounded rows are cleared only when step/state/locale changes.
        display_.fillRect(8, 143, w - 16, h - 146, color_(dark));

        // Keep long translated labels within 320 px without clipping midword.
        const auto drawBounded = [&](const char* label, int16_t x, int16_t baseline,
                                     const GFXfont* mainFont, const GFXfont* smallFont,
                                     uint16_t ink) {
            char bounded[80];
            snprintf(bounded, sizeof(bounded), "%s", label ? label : "");
            setGfxFont_(mainFont, ink, dark);
            const int16_t maxWidth = w - x - 12;
            if (smallFont && gfxTextWidth_(bounded) > maxWidth) {
                setGfxFont_(smallFont, ink, dark);
            }
            if (gfxTextWidth_(bounded) > maxWidth) {
                // Keep the line readable: shorten at a character boundary and
                // show an ellipsis instead of letting the TFT crop the text.
                for (size_t len = strlen(bounded); len > 4U; --len) {
                    bounded[len - 1U] = '\0';
                    char shortened[80];
                    snprintf(shortened, sizeof(shortened), "%s...", bounded);
                    if (gfxTextWidth_(shortened) <= maxWidth) {
                        snprintf(bounded, sizeof(bounded), "%s", shortened);
                        break;
                    }
                }
            }
            display_.setCursor(x, baseline);
            display_.print(bounded);
        };

        // Previous completed stage; the green tick means VERIFIED transition.
        if (updateCompletedStage_[0] && !error) {
            const tft_update::Text completed = tft_update::steps(
                nextion, updateCompletedStage_, english, false, false, zipUpdate);
            display_.drawLine(12, 154, 16, 159, color_(green));
            display_.drawLine(16, 159, 25, 149, color_(green));
            drawBounded(completed.current, 32, 162,
                        &FreeSans9pt7b, nullptr, green);
        }

        // A dark tinted focus strip makes the actual operation unambiguous.
        const uint16_t activeColor = error ? red : cyan;
        display_.fillRoundRect(8, 169, w - 16, 34, 5,
            color_(error ? rgb565_(45, 15, 24) : rgb565_(5, 30, 43)));
        display_.fillRect(11, 176, 3, 20, color_(activeColor));
        drawBounded(done ? (english ? "Update completed" : "Mise a jour terminee") :
                    rebooting ? (english ? "Restarting..." : "Redemarrage...") :
                    stage.current,
                    23, 192, &FreeSansBold9pt7b, &FreeSansBold8pt7b, activeColor);

        const char* detail = error ? tft_update::failureReason(u.message, english) :
                             done ? (english ? "Ready for restart" : "Pret pour redemarrage") :
                             stage.next ? stage.next :
                             (english ? "Finalizing..." : "Finalisation...");
        if (error) {
            display_.drawFastVLine(16, 218, 6, color_(red));
            display_.drawPixel(16, 227, color_(red));
        } else {
            display_.drawCircle(16, 222, 3, color_(faded));
        }
        drawBounded(detail, 30, 229, &FreeSans9pt7b, nullptr,
                    error ? red : faded);
    }
}

void TFTModuleS3::drawBootLogo_()
{
    const int16_t w = display_.width();
    const int16_t h = display_.height();
    // Original splash artwork has a white background; do not darken it.
    display_.fillScreen(color_(rgb565_(255, 255, 255)));
    int16_t x = (int16_t)((w - (int16_t)kFlowIoLogoWidth) / 2);
    int16_t y = (int16_t)(((h - (int16_t)kFlowIoLogoHeight) / 2) - 30);
    if (x < 0) x = 0;
    if (y < 4) y = 4;
    display_.drawRGBBitmap(x, y, kFlowIoLogoBitmap, kFlowIoLogoWidth, kFlowIoLogoHeight);
    int16_t textY = (int16_t)(y + (int16_t)kFlowIoLogoHeight + 8);
    if ((int16_t)(textY + 44) > h) textY = (int16_t)(h - 44);
    drawBrandWordmark_(0, textY, w, 44, kFlowIoWordmarkSplashBitmap,
                       kFlowIoWordmarkSplashWidth, kFlowIoWordmarkSplashHeight);
}

void TFTModuleS3::drawStaticLayout_(Page page)
{
    (void)page;
    if (!layoutDrawn_) {
        // Cold start only. Page rotations do not blank the 32px header.
        display_.fillScreen(color_(kColorBg));
    } else {
        display_.fillRect(0, kHeaderH, display_.width(),
                          (int16_t)(display_.height() - kHeaderH), color_(kColorBg));
    }
}

void TFTModuleS3::drawWifiBars_(int16_t x, int16_t y, uint8_t bars)
{
    // Five enlarged 4px columns, red/orange/yellow/lime/green.
    // Unavailable bars remain dark; neither bitmap nor PSRAM is needed.
    static constexpr uint16_t kWifiColors[] = {
        rgb565_(246, 78, 82), rgb565_(255, 155, 49),
        rgb565_(250, 221, 52), rgb565_(121, 227, 70),
        rgb565_(32, 217, 115)
    };
    for (uint8_t i = 0U; i < 5U; ++i) {
        const int16_t bh = (int16_t)(5 + i * 3);
        display_.fillRoundRect((int16_t)(x + i * 6),
                               (int16_t)(y + 17 - bh), 4, bh, 1,
                               color_(i < bars ? kWifiColors[i] : kColorWifiOff));
    }
}

void TFTModuleS3::drawBrandWordmark_(int16_t x, int16_t y, int16_t w, int16_t h,
                                       const uint16_t* bitmap, int16_t bitmapW, int16_t bitmapH)
{
    const int16_t left = (int16_t)(x + (w - bitmapW) / 2);
    const int16_t top = (int16_t)(y + (h - bitmapH) / 2);
    if (!displayCfg_.swapColorBytes) {
        display_.drawRGBBitmap(left, top, bitmap, bitmapW, bitmapH);
        return;
    }

    // Honour the same colour byte order as the rest of the TFT interface.
    display_.startWrite();
    for (int16_t row = 0; row < bitmapH; ++row) {
        for (int16_t col = 0; col < bitmapW; ++col) {
            const uint16_t pixel = pgm_read_word(bitmap + row * bitmapW + col);
            display_.writePixel(left + col, top + row, color_(pixel));
        }
    }
    display_.endWrite();
}

void TFTModuleS3::drawHeader_(const char* timeText, uint8_t wifiBars,
                              bool apMode, uint8_t activePage, bool full)
{
    const int16_t w = display_.width();
    if (full) {
        // Only first display initialization paints the brand. At each page
        // change redraw just the three navigation dots, not the whole bar.
        if (!layoutDrawn_) {
            display_.fillRect(0, 0, w, kHeaderH, color_(kColorHeader));
            if (!displayCfg_.swapColorBytes) {
                display_.drawRGBBitmap(8, 6, kFlowIoWaveDarkBitmap,
                                       kFlowIoWaveDarkWidth, kFlowIoWaveDarkHeight);
            } else {
                display_.startWrite();
                for (int16_t yy = 0; yy < kFlowIoWaveDarkHeight; ++yy)
                    for (int16_t xx = 0; xx < kFlowIoWaveDarkWidth; ++xx)
                        display_.writePixel(8 + xx, 6 + yy,
                            color_(pgm_read_word(kFlowIoWaveDarkBitmap +
                                                 yy * kFlowIoWaveDarkWidth + xx)));
                display_.endWrite();
            }
            drawGfxText_(&FreeSansBold12pt7b, kColorText, kColorHeader, 48, 25, "flow.io");
            display_.drawFastHLine(8, kHeaderH - 1, w - 16, color_(kColorCardBorder));
        }
        display_.fillRect(142, 8, 36, 16, color_(kColorHeader));
        const uint8_t selected = activePage % 3U;
        for (uint8_t i = 0U; i < 3U; ++i)
            display_.fillCircle((int16_t)(148 + i * 11U), 16, 3,
                color_(i == selected ? kColorSelected : kColorOff));
    }
    // Flicker reduction: Wi-Fi RSSI varies much more often than the clock.
    // Redraw each dirty segment independently; never erase the other one.
    const bool wifiDirty = full || overviewCache_.apMode != apMode ||
                           overviewCache_.wifiBars != wifiBars;
    const bool clockDirty = full ||
        strncmp(overviewCache_.timeText, timeText ? timeText : "--:--",
                sizeof(overviewCache_.timeText)) != 0;
    if (wifiDirty) {
        display_.fillRect(w - 96, 1, 38, kHeaderH - 3, color_(kColorHeader));
        if (apMode) {
            drawGfxText_(&FreeSans9pt7b, kColorOn, kColorHeader,
                         (int16_t)(w - 91), 23, "AP");
        } else {
            drawWifiBars_((int16_t)(w - 90), 7, wifiBars);
        }
    }
    if (!clockDirty) return;
    display_.fillRect(w - 58, 1, 58, kHeaderH - 3, color_(kColorHeader));
    // Regular weight clock, +2 pixels vertically and right-aligned.
    setGfxFont_(&FreeSans9pt7b, kColorText, kColorHeader);
    const char* cleanTime = (timeText && timeText[0] != '\0') ? timeText : "--:--";
    const int16_t tw = gfxTextWidth_(cleanTime);
    const GFXfont* clockFont = &FreeSans9pt7b;
    const GFXglyph* glyphs = clockFont->glyph;
    const uint8_t* bitmap = clockFont->bitmap;
    int16_t glyphX = (int16_t)(w - tw - 8);
    constexpr int16_t kClockBaseline = 22;
    for (const char* cp = cleanTime; *cp; ++cp) {
        const uint8_t c = (uint8_t)*cp;
        if (c < clockFont->first || c > clockFont->last) continue;
        const GFXglyph* glyph = &glyphs[c - clockFont->first];
        const uint16_t offset = pgm_read_word(&glyph->bitmapOffset);
        const uint8_t gw = pgm_read_byte(&glyph->width);
        const uint8_t gh = pgm_read_byte(&glyph->height);
        const int8_t xo = (int8_t)pgm_read_byte(&glyph->xOffset);
        const int8_t yo = (int8_t)pgm_read_byte(&glyph->yOffset);
        const uint8_t advance = pgm_read_byte(&glyph->xAdvance);
        if (gh > 0U && gw > 0U) {
            for (uint8_t dy = 0U; dy < (uint8_t)(gh + 2U); ++dy) {
                const uint8_t srcY = (uint8_t)((uint16_t)dy * gh / (gh + 2U));
                for (uint8_t dx = 0U; dx < gw; ++dx) {
                    const uint16_t bit = (uint16_t)srcY * gw + dx;
                    const uint8_t bits = pgm_read_byte(bitmap + offset + (bit >> 3U));
                    if (bits & (uint8_t)(0x80U >> (bit & 7U)))
                        display_.drawPixel((int16_t)(glyphX + xo + dx),
                                           (int16_t)(kClockBaseline + yo - 1 + dy),
                                           color_(kColorText));
                }
            }
        }
        glyphX = (int16_t)(glyphX + advance);
    }
}

void TFTModuleS3::drawCenteredTextCard_(int16_t x, int16_t y, int16_t w, int16_t h,
                                         const char* text, bool alignLeft)
{
    display_.fillRoundRect(x, y, w, h, kUiCardRadius, color_(kColorCardBg));
    display_.drawRoundRect(x, y, w, h, kUiCardRadius, color_(kColorCardBorder));
    const char* value = (text && text[0] != '\0') ? text : "--";

    // IP and URL are BOTH centered horizontally and vertically inside their
    // own 158x41 cards. Use the largest font that fits the real 320px panel.
    const GFXfont* font = &FreeSans9pt7b;
    setGfxFont_(font, kColorText, kColorCardBg);
    int16_t tw = gfxTextWidth_(value);
    if (tw > w - 8) {
        font = &FreeSansBold8pt7b;
        setGfxFont_(font, kColorText, kColorCardBg);
        tw = gfxTextWidth_(value);
    }
    if (tw <= w - 8) {
        const int16_t textX = alignLeft ? (int16_t)(x + 5)
                                        : (int16_t)(x + (w - tw) / 2);
        drawGfxText_(font, kColorText, kColorCardBg, textX,
                     gfxBaselineCenteredInBox_(value, y, h), value);
        return;
    }
    // Safe fallback for unusually long hostnames; bounded within the frame.
    char shortText[44]{};
    snprintf(shortText, sizeof(shortText), "%s", value);
    const size_t maxChars = (size_t)((w - 8) / 6);
    if (maxChars < sizeof(shortText)) shortText[maxChars] = '\0';
    const int16_t width = (int16_t)(strlen(shortText) * 6U);
    display_.setFont(nullptr);
    display_.setTextSize(1);
    display_.setTextColor(color_(kColorText), color_(kColorCardBg));
    display_.setCursor(alignLeft ? (int16_t)(x + 5)
                                 : (int16_t)(x + (w - width) / 2),
                       (int16_t)(y + (h - 8) / 2));
    display_.print(shortText);
}

void TFTModuleS3::drawAlarmIndicator_(int16_t cx, int16_t cy, uint16_t fill, bool active)
{
    display_.fillCircle(cx, cy, 11, color_(fill));
    const uint16_t markColor = color_(kColorCheckMark);
    if (!active) {
        for (int8_t off = -1; off <= 1; ++off) {
            display_.drawLine((int16_t)(cx - 5), (int16_t)(cy + off), (int16_t)(cx - 1), (int16_t)(cy + 4 + off), markColor);
            display_.drawLine((int16_t)(cx - 1), (int16_t)(cy + 4 + off), (int16_t)(cx + 6), (int16_t)(cy - 4 + off), markColor);
        }
        return;
    }
    display_.fillRoundRect((int16_t)(cx - 2), (int16_t)(cy - 7), 4, 10, 2, markColor);
    display_.fillCircle(cx, (int16_t)(cy + 6), 2, markColor);
}

void TFTModuleS3::drawMiniIcon_(int16_t cx, int16_t cy, uint8_t kind, uint16_t bg)
{
    // 16x16, vivid vector pictograms; no new images, fonts or heap allocations.
    // The full icon stays inside +/-8 pixels for dense 5x2 cards.
    const uint16_t azure = color_(rgb565_(30, 180, 246));
    const uint16_t light = color_(rgb565_(128, 237, 255));
    const uint16_t yellow = color_(rgb565_(255, 213, 36));
    const uint16_t green = color_(rgb565_(38, 223, 128));
    const uint16_t violet = color_(rgb565_(190, 133, 255));
    const uint16_t red = color_(rgb565_(254, 100, 63));
    const uint16_t background = color_(bg);
    switch (kind) {
        case 0U: // Mode: blue gear
            display_.drawCircle(cx, cy, 6, light);
            display_.fillCircle(cx, cy, 2, azure);
            display_.drawFastVLine(cx, cy - 8, 3, light);
            display_.drawFastVLine(cx, cy + 6, 3, light);
            display_.drawFastHLine(cx - 8, cy, 3, light);
            display_.drawFastHLine(cx + 6, cy, 3, light);
            break;
        case 1U: // Water temperature thermometer: orange/red
        case 14U: // Air temperature thermometer: ice blue
        {
            const uint16_t inner = kind == 14U ? azure : red;
            display_.drawRoundRect(cx - 3, cy - 8, 7, 13, 3, light);
            display_.fillCircle(cx, cy + 5, 3, inner);
            display_.drawFastVLine(cx, cy - 4, 8, inner);
            break;
        }
        case 2U: // Waterfall: crest and two free-flowing cyan streams
            display_.drawFastHLine(cx - 7, cy - 7, 12, green);
            display_.drawLine(cx - 5, cy - 6, cx - 4, cy + 4, azure);
            display_.drawLine(cx - 4, cy - 6, cx - 2, cy + 5, light);
            display_.drawLine(cx, cy - 6, cx + 1, cy + 5, azure);
            display_.drawLine(cx + 3, cy - 6, cx + 5, cy + 4, light);
            display_.drawFastHLine(cx - 7, cy + 7, 15, light);
            break;
        case 3U:  // PSI/barometer value
        case 11U: // Low pressure, needle at start (left)
        case 12U: // High pressure, needle at end (right)
        case 13U: // Ambient pressure
        {
            display_.drawCircle(cx, cy + 1, 7, light);
            display_.drawLine(cx - 6, cy - 2, cx - 4, cy - 5, green);
            display_.drawLine(cx - 1, cy - 6, cx + 2, cy - 6, yellow);
            display_.drawLine(cx + 5, cy - 4, cx + 7, cy - 1, red);
            const int16_t tx = kind == 11U ? cx - 5 : cx + 5;
            const int16_t ty = kind == 13U ? cy - 5 : cy - 3;
            display_.drawLine(cx, cy + 2, tx, ty, kind == 13U ? violet : azure);
            display_.fillCircle(cx, cy + 2, 2, light);
            break;
        }
        case 4U: // Water meter
            display_.fillRoundRect(cx - 7, cy - 6, 14, 12, 3, light);
            display_.fillRect(cx - 5, cy - 3, 10, 5, background);
            display_.drawFastHLine(cx - 4, cy + 4, 8, azure);
            break;
        case 5U: // pH lab flask: bright green liquid
            display_.drawFastHLine(cx - 3, cy - 8, 7, light);
            display_.fillRect(cx - 2, cy - 7, 4, 6, light);
            display_.fillTriangle(cx - 2, cy - 1, cx - 8, cy + 7, cx + 8, cy + 7, green);
            display_.drawFastHLine(cx - 8, cy + 7, 17, light);
            break;
        case 6U: // ORP lightning
            display_.fillTriangle(cx + 2, cy - 8, cx - 6, cy + 1, cx + 1, cy + 1, yellow);
            display_.fillTriangle(cx - 1, cy - 1, cx + 7, cy - 1, cx - 3, cy + 8, yellow);
            break;
        case 7U: // Pump / impeller
            display_.drawCircle(cx, cy, 7, violet);
            display_.fillCircle(cx, cy, 2, light);
            display_.fillTriangle(cx, cy - 2, cx - 4, cy - 7, cx + 4, cy - 6, violet);
            display_.fillTriangle(cx + 2, cy, cx + 7, cy + 3, cx + 2, cy + 6, violet);
            display_.fillTriangle(cx - 2, cy + 2, cx - 7, cy + 4, cx - 6, cy - 4, violet);
            break;
        case 8U: // Water/chemical level droplet
            display_.fillTriangle(cx, cy - 8, cx - 6, cy + 2, cx + 6, cy + 2, azure);
            display_.fillCircle(cx, cy + 2, 6, azure);
            display_.fillCircle(cx - 2, cy + 2, 1, light);
            break;
        case 9U: // Uptime clock
            display_.drawCircle(cx, cy, 7, violet);
            display_.drawFastVLine(cx, cy - 5, 6, light);
            display_.drawFastHLine(cx, cy, 5, yellow);
            break;
        case 15U: // Low water level: waves, not the flow-rate waterfall
            for (int16_t yy = -3; yy <= 4; yy += 6) {
                display_.drawLine(cx - 8, cy + yy, cx - 4, cy + yy - 1, azure);
                display_.drawLine(cx - 4, cy + yy - 1, cx, cy + yy + 1, light);
                display_.drawLine(cx, cy + yy + 1, cx + 4, cy + yy - 1, azure);
                display_.drawLine(cx + 4, cy + yy - 1, cx + 8, cy + yy, light);
            }
            break;
        case 16U: // Chlorine canister with blue tank/body
            display_.fillRoundRect(cx - 6, cy - 5, 12, 12, 2, azure);
            display_.drawFastHLine(cx - 2, cy - 8, 6, light);
            display_.drawFastVLine(cx - 2, cy - 7, 3, light);
            display_.drawFastVLine(cx + 3, cy - 7, 3, light);
            display_.drawFastHLine(cx - 3, cy + 2, 6, light);
            break;
        case 17U: // BME sensor IC: lime pins / cyan silicon
            display_.fillRoundRect(cx - 5, cy - 5, 10, 10, 2, green);
            display_.fillRoundRect(cx - 3, cy - 3, 6, 6, 1, background);
            display_.fillCircle(cx, cy, 1, light);
            for (int16_t q = -4; q <= 4; q += 4) {
                display_.drawFastHLine(cx - 8, cy + q, 3, green);
                display_.drawFastHLine(cx + 6, cy + q, 3, green);
                display_.drawFastVLine(cx + q, cy - 8, 3, green);
                display_.drawFastVLine(cx + q, cy + 6, 3, green);
            }
            break;
        case 18U: // Water volume tank: turquoise waves
            display_.drawRoundRect(cx - 7, cy - 7, 14, 15, 3, light);
            display_.fillRoundRect(cx - 6, cy + 1, 12, 6, 2, azure);
            display_.drawFastHLine(cx - 4, cy + 3, 8, light);
            break;
        case 19U: // Electrical voltage: green badge + yellow bolt
            display_.fillCircle(cx, cy, 7, green);
            display_.fillTriangle(cx + 1, cy - 6, cx - 4, cy + 1,
                                  cx + 1, cy + 1, yellow);
            display_.fillTriangle(cx - 1, cy - 1, cx + 4, cy - 1,
                                  cx - 2, cy + 6, yellow);
            break;
        default: // Neutral information
            display_.fillCircle(cx, cy, 7, azure);
            display_.fillCircle(cx, cy - 3, 1, light);
            display_.fillRoundRect(cx - 1, cy - 1, 2, 6, 0, light);
            break;
    }
}

void TFTModuleS3::drawOverviewSelectorRow_(uint8_t row, const OverviewChoice* choices,
                                           uint8_t count, uint8_t selected, bool available)
{
    static constexpr const char* kRowCaptions[] = {"Mode", "Correction pH", "Traitement"};
    static constexpr uint16_t kRowColors[] = {
        kColorOverviewModeBg, kColorOverviewPhBg, kColorOverviewTreatmentBg
    };
    if (!choices || !count || row >= kOverviewSelectorRows) return;
    const Rect rect = overviewStatusCardRect_(display_.width(), display_.height(), row);
    const uint16_t bg = kRowColors[row];
    display_.fillRoundRect(rect.x, rect.y, rect.w, rect.h, kUiCardRadius, color_(bg));
    display_.drawRoundRect(rect.x, rect.y, rect.w, rect.h, kUiCardRadius,
                           color_(kColorCardBorder));

    // A summary, not a row of controls: show only the saved, selected value.
    const char* current = "--";
    if (available) {
        for (uint8_t i = 0U; i < count; ++i) {
            if (choices[i].value == selected) {
                current = choices[i].label;
                break;
            }
        }
    }
    const int16_t centerY = (int16_t)(rect.y + rect.h / 2);
    const int16_t iconX = (int16_t)(rect.x + 24);
    // More colourful and larger icons for the 53px-high overview cards:
    // blue gear/yellow axle, green chemistry flask, turquoise water droplet.
    if (row == 0U) {
        const uint16_t sky = color_(rgb565_(45, 193, 252));
        const uint16_t yellow = color_(rgb565_(255, 215, 83));
        for (int16_t dx = -1; dx <= 1; ++dx) {
            display_.drawFastVLine((int16_t)(iconX + dx), (int16_t)(centerY - 12), 4, sky);
            display_.drawFastVLine((int16_t)(iconX + dx), (int16_t)(centerY + 9), 4, sky);
            display_.drawFastHLine((int16_t)(iconX - 12), (int16_t)(centerY + dx), 4, sky);
            display_.drawFastHLine((int16_t)(iconX + 9), (int16_t)(centerY + dx), 4, sky);
        }
        display_.drawCircle(iconX, centerY, 9, sky);
        display_.drawCircle(iconX, centerY, 8, sky);
        display_.fillCircle(iconX, centerY, 3, yellow);
    } else if (row == 1U) {
        const uint16_t glass = color_(rgb565_(204, 246, 255));
        const uint16_t fluid = color_(rgb565_(18, 211, 110));
        display_.drawFastHLine((int16_t)(iconX - 4), (int16_t)(centerY - 11), 9, glass);
        display_.drawLine(iconX - 3, centerY - 10, iconX - 3, centerY - 2, glass);
        display_.drawLine(iconX + 3, centerY - 10, iconX + 3, centerY - 2, glass);
        display_.fillTriangle(iconX - 3, centerY - 2,
                              iconX - 11, centerY + 11,
                              iconX + 11, centerY + 11, fluid);
        display_.drawLine(iconX + 3, centerY - 2, iconX + 11, centerY + 11, glass);
        display_.drawFastHLine(iconX - 11, centerY + 11, 23, glass);
        display_.fillCircle(iconX - 2, centerY + 5, 2, color_(rgb565_(148, 255, 229)));
        display_.fillCircle(iconX + 4, centerY + 7, 1, color_(rgb565_(255, 223, 82)));
    } else {
        const uint16_t blue = color_(rgb565_(13, 160, 248));
        const uint16_t cyan = color_(rgb565_(26, 229, 217));
        display_.fillTriangle(iconX, centerY - 13, iconX - 10, centerY + 3,
                              iconX + 10, centerY + 3, blue);
        display_.fillCircle(iconX, centerY + 5, 10, cyan);
        display_.fillCircle(iconX - 3, centerY + 5, 2, color_(rgb565_(220, 253, 255)));
    }
    const int16_t titleX = (int16_t)(rect.x + 47);
    drawGfxText_(&FreeSans9pt7b, kColorText, bg,
                 titleX, (int16_t)(centerY + 6), kRowCaptions[row]);

    const uint16_t valueColor = available ? kColorText : kColorMuted;
    // Compact regular 9pt for the selected mode: consistent with card labels.
    const GFXfont* valueFont = &FreeSans9pt7b;
    setGfxFont_(valueFont, valueColor, bg);
    const int16_t valueW = gfxTextWidth_(current);
    const int16_t valueX = (int16_t)(rect.x + rect.w - valueW - 12);
    drawGfxText_(valueFont, valueColor, bg, valueX,
                 (int16_t)(centerY + 6), current);
}

void TFTModuleS3::drawOverviewSelectors_(const OverviewStatusRenderState& state)
{
    static constexpr OverviewChoice kOperatingChoices[] = {
        {"Manuel", (uint8_t)PoolOperatingMode::Manual},
        {"Auto", (uint8_t)PoolOperatingMode::Automatic},
        {"Hiver", (uint8_t)PoolOperatingMode::Winter},
    };
    static constexpr OverviewChoice kPhChoices[] = {
        {"pH+", (uint8_t)PoolPhCorrection::Plus},
        {"pH-", (uint8_t)PoolPhCorrection::Minus},
        {"Off", (uint8_t)PoolPhCorrection::Disabled},
    };
    // The built-in FreeSans font contains ASCII glyphs: use O2, not a subscript.
    static constexpr OverviewChoice kTreatmentChoices[] = {
        {"Chlore", (uint8_t)PoolDisinfectionMethod::ChlorineBromine},
        {"Electro", (uint8_t)PoolDisinfectionMethod::SaltElectrolysis},
        {"O2", (uint8_t)PoolDisinfectionMethod::ActiveOxygen},
        {"Off", (uint8_t)PoolDisinfectionMethod::Disabled},
    };
    drawOverviewSelectorRow_(0U, kOperatingChoices, sizeof(kOperatingChoices) / sizeof(kOperatingChoices[0]),
                             (uint8_t)state.operatingMode, state.operatingAvailable);
    drawOverviewSelectorRow_(1U, kPhChoices, sizeof(kPhChoices) / sizeof(kPhChoices[0]),
                             (uint8_t)state.phCorrection, state.operatingAvailable);
    drawOverviewSelectorRow_(2U, kTreatmentChoices, sizeof(kTreatmentChoices) / sizeof(kTreatmentChoices[0]),
                             (uint8_t)state.treatment, state.treatmentAvailable);
}

bool TFTModuleS3::overviewStatusEquals_(const OverviewStatusRenderState& a, const OverviewStatusRenderState& b)
{
    return a.operatingAvailable == b.operatingAvailable &&
           a.treatmentAvailable == b.treatmentAvailable &&
           a.operatingMode == b.operatingMode && a.phCorrection == b.phCorrection && a.treatment == b.treatment;
}

void TFTModuleS3::drawAlarmCard_(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t startSlot)
{
    uint32_t activeMask = 0U;
    uint32_t resettableMask = 0U;
    uint32_t conditionMask = 0U;
    loadAlarmMasks_(activeMask, resettableMask, conditionMask);
    (void)resettableMask;

    display_.fillRoundRect(x, y, w, h, kUiCardRadius, color_(kColorCardBg));
    display_.drawRoundRect(x, y, w, h, kUiCardRadius, color_(kColorCardBorder));

    const int16_t iconCx = (int16_t)(x + w - 22);
    const int16_t labelX = (int16_t)(x + 12);
    const int16_t topPad = 18;
    const int16_t bottomPad = 18;
    const int16_t rowSpan = (int16_t)(h - topPad - bottomPad);
    const int16_t rowGap = (int16_t)(rowSpan / 3);
    for (uint8_t i = 0U; i < 4U; ++i) {
        const uint8_t alarmIndex = (uint8_t)(startSlot + i);
        if (alarmIndex >= kAlarmSlotCount) break;
        const uint32_t mask = (alarmIndex < 32U) ? (1UL << alarmIndex) : 0U;
        const char* label = runtimeUiAlarmActiveFlagLabel(mask, "fr");
        if (!label || label[0] == '\0') label = "Alarme";
        const int16_t rowCenterY = (int16_t)(y + topPad + ((int16_t)i * rowGap));
        const uint16_t textColor = alarmTextColor_(activeMask, alarmIndex);
        const uint16_t indicatorColor = alarmIndicatorColor_(activeMask, conditionMask, alarmIndex);
        const bool active = (activeMask & mask) != 0U;
        drawGfxTextCenteredY_(&FreeSans9pt7b, textColor, kColorCardBg, labelX, (int16_t)(rowCenterY - 12), 24, label);
        drawAlarmIndicator_(iconCx, rowCenterY, indicatorColor, active);
    }
}

void TFTModuleS3::drawOverviewPage_()
{
    const int16_t w = display_.width();
    char ip[20] = {0};
    formatIp_(ip, sizeof(ip));

    const Rect leftTop = overviewTopCardRect_(w, 0U);
    const Rect rightTop = overviewTopCardRect_(w, 1U);
    OverviewStatusRenderState status{};
    readOverviewStatus_(status);
    drawCenteredTextCard_(leftTop.x, leftTop.y, leftTop.w, leftTop.h, ip, false);
    drawCenteredTextCard_(rightTop.x, rightTop.y, rightTop.w, rightTop.h, "http://flowio.local", false);
    drawOverviewSelectors_(status);
}

void TFTModuleS3::drawMetricsPage_()
{
    const int16_t w = display_.width();
    const int16_t h = display_.height();
    uint8_t visibleIndex = 0U;
    const bool denseMetrics = true; // Always preserve the approved 5x2 screen-2 grid.
    for (uint8_t i = 0; i < DashboardSlotCount; ++i) {
        if (!uiStorage_->dashboardCfg[i].enabled) continue;
        const Rect r = metricCardRect_(w, h, visibleIndex++, denseMetrics);
        drawDashboardSlot_(i, r.x, r.y, r.w, r.h, denseMetrics);
    }
}

void TFTModuleS3::drawAlarmsPage_()
{
    const int16_t w = display_.width();
    const int16_t h = display_.height();
    for (uint8_t i = 0; i < AlarmDashboardSlotCount; ++i) {
        const Rect r = metricCardRect_(w, h, i);
        drawAlarmDashboardSlot_(i, r.x, r.y, r.w, r.h);
    }
}

void TFTModuleS3::drawDashboardSlot_(uint8_t slot, int16_t x, int16_t y, int16_t w, int16_t h, bool dense)
{
    DashboardSlotRenderState state{};
    readDashboardSlotState_(slot, state);
    drawDashboardSlot_(slot, x, y, w, h, state, dense);
}

void TFTModuleS3::drawDashboardSlot_(uint8_t slot, int16_t x, int16_t y, int16_t w, int16_t h,
                                      const DashboardSlotRenderState& state,
                                      bool dense, bool textOnly)
{
    (void)slot;
    (void)dense;
    const uint16_t bg = color_(state.cardBg);
    if (!textOnly) {
        display_.fillRoundRect(x, y, w, h, kUiCardRadius, bg);
        display_.drawRoundRect(x, y, w, h, kUiCardRadius, color_(kColorCardBorder));
        if (state.enabled) {
            // No decorative divider: use the space for larger labels.
            drawMiniIcon_((int16_t)(x + 13), (int16_t)(y + h / 2),
                          metricGlyphKind_(state.label, state.unit), state.cardBg);
        }
    } else {
        // Anti-flicker: erase only changed text, keeping icon and frame.
        display_.fillRect((int16_t)(x + 26), (int16_t)(y + 3),
                          (int16_t)(w - 30), (int16_t)(h - 6), bg);
    }
    if (!state.enabled) return;

    const uint16_t valueColor = state.available ? kColorValue : kColorMuted;
    const char* value = state.value[0] ? state.value : "--";
    const bool hasUnit = state.unit[0] != '\0';
    const bool degreeC = hasUnit && isDegreeCUnit_(state.unit);
    const char* unitText = degreeC ? "C" : state.unit;
    const int16_t unitWidth = hasUnit ? (int16_t)(strlen(unitText) * 6 + (degreeC ? 6 : 0)) : 0;

    // Right-align value immediately before the unit. This grants the label
    // all remaining horizontal space and preserves the 5x2 native layout.
    const int16_t rightEdge = (int16_t)(x + w - 6);
    const int16_t unitX = (int16_t)(rightEdge - unitWidth);
    const int16_t valueRight = (int16_t)(unitX - (hasUnit ? 5 : 0));
    setGfxFont_(&FreeSans9pt7b, valueColor, state.cardBg);
    int16_t valueWidth = gfxTextWidth_(value);
    const int16_t labelX = (int16_t)(x + 27);
    const int16_t maxValueSpan = (int16_t)(valueRight - labelX - 18);
    const bool compactValue = valueWidth > maxValueSpan;
    if (compactValue) valueWidth = (int16_t)(strlen(value) * 6U);
    const int16_t valueX = (int16_t)(valueRight - valueWidth);

    // Label receives the space left of the numeric value. If it contains a
    // space, split once, then clip each segment to avoid printing over the
    // value/units. Never mutate the configured label stored in NVS.
    char labelBuf[sizeof(state.label)]{};
    snprintf(labelBuf, sizeof(labelBuf), "%s", state.label);
    int16_t labelMaxW = (int16_t)(valueX - labelX - 4);
    if (labelMaxW < 12) labelMaxW = 12;
    const int16_t midY = (int16_t)(y + h / 2);
    setGfxFont_(&FreeSans9pt7b, kColorText, state.cardBg);
    if (gfxTextWidth_(labelBuf) <= labelMaxW) {
        drawGfxText_(&FreeSans9pt7b, kColorText, state.cardBg,
                     labelX, (int16_t)(midY + 6), labelBuf);
    } else {
        char* space = strchr(labelBuf, ' ');
        // Keep 8pt GFX when possible; fall back to compact 6x8 only
        // for longer user-defined names (never overwrite stored labels).
        if (space) {
            *space++ = '\0';
            char second[sizeof(state.label)]{};
            snprintf(second, sizeof(second), "%s", space);
            setGfxFont_(&FreeSansBold8pt7b, kColorText, state.cardBg);
            if (gfxTextWidth_(labelBuf) <= labelMaxW &&
                gfxTextWidth_(second) <= labelMaxW) {
                drawGfxText_(&FreeSansBold8pt7b, kColorText, state.cardBg,
                             labelX, (int16_t)(midY - 2), labelBuf);
                drawGfxText_(&FreeSansBold8pt7b, kColorText, state.cardBg,
                             labelX, (int16_t)(midY + 12), second);
            } else {
                display_.setFont(nullptr);
                display_.setTextSize(1);
                display_.setTextColor(color_(kColorText), bg);
                const size_t maxChars = (size_t)(labelMaxW / 6);
                if (maxChars < sizeof(labelBuf)) labelBuf[maxChars] = '\0';
                if (maxChars < sizeof(second)) second[maxChars] = '\0';
                display_.setCursor(labelX, (int16_t)(midY - 9));
                display_.print(labelBuf);
                display_.setCursor(labelX, (int16_t)(midY + 2));
                display_.print(second);
            }
        } else {
            setGfxFont_(&FreeSansBold8pt7b, kColorText, state.cardBg);
            if (gfxTextWidth_(labelBuf) <= labelMaxW) {
                drawGfxText_(&FreeSansBold8pt7b, kColorText, state.cardBg,
                             labelX, (int16_t)(midY + 5), labelBuf);
            } else {
                display_.setFont(nullptr);
                display_.setTextSize(1);
                display_.setTextColor(color_(kColorText), bg);
                const size_t maxChars = (size_t)(labelMaxW / 6);
                if (maxChars < sizeof(labelBuf)) labelBuf[maxChars] = '\0';
                display_.setCursor(labelX, (int16_t)(midY - 4));
                display_.print(labelBuf);
            }
        }
    }

    // Value and unit remain right-aligned on the same baseline.
    if (!compactValue) {
        setGfxFont_(&FreeSans9pt7b, valueColor, state.cardBg);
        display_.setCursor(valueX, (int16_t)(midY + 6));
    } else {
        display_.setFont(nullptr);
        display_.setTextSize(1);
        display_.setTextColor(color_(valueColor), bg);
        display_.setCursor(valueX, (int16_t)(midY - 4));
    }
    display_.print(value);
    if (hasUnit) {
        display_.setFont(nullptr);
        display_.setTextSize(1);
        display_.setTextColor(color_(kColorSelected), bg);
        if (degreeC) {
            display_.drawCircle((int16_t)(unitX + 2), (int16_t)(midY - 4),
                                2, color_(kColorSelected));
            display_.setCursor((int16_t)(unitX + 6), (int16_t)(midY - 4));
            display_.print("C");
        } else {
            display_.setCursor(unitX, (int16_t)(midY - 4));
            display_.print(unitText);
        }
    }
}

void TFTModuleS3::readDashboardSlotState_(uint8_t slot, DashboardSlotRenderState& state) const
{
    state = DashboardSlotRenderState{};
    if (slot >= DashboardSlotCount) return;

    const DashboardSlotConfig& cfg = uiStorage_->dashboardCfg[slot];
    state.valid = true;
    state.enabled = cfg.enabled;
    state.cardBg = cfg.enabled ? dashboardColor_(cfg.colorId, slot) : kColorCardBg;
    slotLabel_(slot, state.label, sizeof(state.label));

    RuntimeValue rv{};
    state.available = cfg.enabled &&
                      cfg.runtimeUiId != 0U &&
                      runtimeUiAllowed_(cfg.runtimeUiId) &&
                      readRuntimeValue_(cfg.runtimeUiId, rv);
    rv.available = state.available;
    formatRuntimeValue_(cfg.runtimeUiId, rv, state.value, sizeof(state.value), state.unit, sizeof(state.unit));
}

void TFTModuleS3::drawAlarmDashboardSlot_(uint8_t slot, int16_t x, int16_t y, int16_t w, int16_t h)
{
    AlarmSlotRenderState state{};
    readAlarmSlotState_(slot, state);
    drawAlarmDashboardSlot_(slot, x, y, w, h, state);
}

void TFTModuleS3::drawAlarmDashboardSlot_(uint8_t slot, int16_t x, int16_t y,
                                           int16_t w, int16_t h,
                                           const AlarmSlotRenderState& state)
{
    (void)slot;
    if (!state.enabled) {
        display_.fillRect(x, y, w, h, color_(kColorBg));
        return;
    }
    const uint16_t bg = color_(state.cardBg);
    display_.fillRoundRect(x, y, w, h, kUiCardRadius, bg);
    display_.drawRoundRect(x, y, w, h, kUiCardRadius, color_(kColorCardBorder));
    // Keep only one narrow indicator column: L above C on the right.
    // No divider: colorful icon and label sit directly in the card.
    drawMiniIcon_((int16_t)(x + 16), (int16_t)(y + h / 2),
                  alarmGlyphKind_(state.alarmId), state.cardBg);

    const int16_t labelX = (int16_t)(x + 32);
    const int16_t labelW = (int16_t)(w - 58);
    const char* text = state.label[0] != '\0' ? state.label : "Alarme";
    setGfxFont_(&FreeSans9pt7b, kColorText, state.cardBg);
    if (gfxTextWidth_(text) <= labelW) {
        drawGfxText_(&FreeSans9pt7b, kColorText, state.cardBg,
                     labelX, (int16_t)(y + h / 2 + 6), text);
    } else {
        char labelBuf[sizeof(state.label)]{};
        snprintf(labelBuf, sizeof(labelBuf), "%s", text);
        char* space = strchr(labelBuf, ' ');
        // Prefer visible 8pt GFX text on long names, splitting only at
        // a real word boundary; use 6x8 only for extreme custom captions.
        if (space) {
            *space++ = '\0';
            char second[sizeof(state.label)]{};
            snprintf(second, sizeof(second), "%s", space);
            setGfxFont_(&FreeSansBold8pt7b, kColorText, state.cardBg);
            if (gfxTextWidth_(labelBuf) <= labelW &&
                gfxTextWidth_(second) <= labelW) {
                drawGfxText_(&FreeSansBold8pt7b, kColorText, state.cardBg,
                             labelX, (int16_t)(y + h / 2 - 2), labelBuf);
                drawGfxText_(&FreeSansBold8pt7b, kColorText, state.cardBg,
                             labelX, (int16_t)(y + h / 2 + 12), second);
            } else {
                display_.setFont(nullptr);
                display_.setTextSize(1);
                display_.setTextColor(color_(kColorText), bg);
                const size_t maxChars = (size_t)(labelW / 6);
                if (maxChars < sizeof(labelBuf)) labelBuf[maxChars] = '\0';
                if (maxChars < sizeof(second)) second[maxChars] = '\0';
                display_.setCursor(labelX, (int16_t)(y + h / 2 - 9));
                display_.print(labelBuf);
                display_.setCursor(labelX, (int16_t)(y + h / 2 + 2));
                display_.print(second);
            }
        } else {
            setGfxFont_(&FreeSansBold8pt7b, kColorText, state.cardBg);
            if (gfxTextWidth_(labelBuf) <= labelW) {
                drawGfxText_(&FreeSansBold8pt7b, kColorText, state.cardBg,
                             labelX, (int16_t)(y + h / 2 + 5), labelBuf);
            } else {
                display_.setFont(nullptr);
                display_.setTextSize(1);
                display_.setTextColor(color_(kColorText), bg);
                const size_t maxChars = (size_t)(labelW / 6);
                if (maxChars < sizeof(labelBuf)) labelBuf[maxChars] = '\0';
                display_.setCursor(labelX, (int16_t)(y + h / 2 - 4));
                display_.print(labelBuf);
            }
        }
    }

    const int16_t stateX = (int16_t)(x + w - 13);
    drawAlarmStateIcon_(stateX, (int16_t)(y + 13), "L",
                        state.latched, state.available);
    drawAlarmStateIcon_(stateX, (int16_t)(y + h - 13), "C",
                        state.conditionTrue, state.available && state.conditionKnown);
}

void TFTModuleS3::drawAlarmStateIcon_(int16_t cx, int16_t cy, const char* text,
                                      bool on, bool known)
{
    const uint16_t fill = !known ? kColorOff : (on ? kColorAlarmAct : kColorGaugeOk);
    // 14px circles, each redrawn independently without repainting the card.
    display_.fillCircle(cx, cy, 7, color_(fill));
    display_.drawCircle(cx, cy, 7, color_(kColorText));
    display_.setFont(nullptr);
    display_.setTextSize(1);
    display_.setTextColor(color_(kColorCheckMark), color_(fill));
    display_.setCursor((int16_t)(cx - 3), (int16_t)(cy - 3));
    display_.print(text ? text : "");
}

void TFTModuleS3::readAlarmSlotState_(uint8_t slot, AlarmSlotRenderState& state) const
{
    state = AlarmSlotRenderState{};
    if (slot >= AlarmDashboardSlotCount) return;

    const AlarmSlotConfig& cfg = uiStorage_->alarmDashboardCfg[slot];
    state.valid = true;
    state.enabled = cfg.enabled;
    state.alarmId = cfg.alarmId;
    state.cardBg = cfg.enabled ? dashboardColor_(cfg.colorId, slot) : kColorCardBg;
    if (!cfg.enabled) return;

    if (cfg.label[0] != '\0') {
        snprintf(state.label, sizeof(state.label), "%s", cfg.label);
    } else {
        snprintf(state.label, sizeof(state.label), "%s", alarmIdLabel_(cfg.alarmId));
    }

    if (cfg.enabled && cfg.alarmId != 0U) {
        (void)readAlarmRuntimeState_(cfg.alarmId, state);
    }
}

void TFTModuleS3::formatIp_(char* out, size_t outLen) const
{
    if (!out || outLen == 0U) return;
    const DataStore* ds = dsSvc_ ? dsSvc_->store : nullptr;
    if (!ds || !networkReady(*ds)) {
        snprintf(out, outLen, "-");
        return;
    }
    const IpV4 ip = networkIp(*ds);
    snprintf(out,
             outLen,
             "%u.%u.%u.%u",
             (unsigned)ip.b[0],
             (unsigned)ip.b[1],
             (unsigned)ip.b[2],
             (unsigned)ip.b[3]);
}

bool TFTModuleS3::readRuntimeValue_(RuntimeUiId runtimeId, char* out, size_t outLen) const
{
    if (!out || outLen == 0U) return false;
    RuntimeValue value{};
    if (!readRuntimeValue_(runtimeId, value)) return false;
    char unit[8] = {0};
    formatRuntimeValue_(runtimeId, value, out, outLen, unit, sizeof(unit));
    if (unit[0] != '\0') {
        const size_t n = strnlen(out, outLen);
        if (n + 1U < outLen) snprintf(out + n, outLen - n, " %s", unit);
    }
    return out[0] != '\0';
}

bool TFTModuleS3::readRuntimeValue_(RuntimeUiId runtimeId, RuntimeValue& out) const
{
    if (!runtimeUiAllowed_(runtimeId)) return false;
    const DataStore* ds = dsSvc_ ? dsSvc_->store : nullptr;
    if (runtimeDataValueId(runtimeId) != VALUE_INVALID) {
        ValueMetadata metadata{};
        return ds && presentDataValue(ds->values, runtimeId, out, metadata);
    }
    const ModuleId module = (ModuleId)runtimeUiModuleId(runtimeId);
    const uint8_t valueId = runtimeUiValueId(runtimeId);

    switch (module) {
        case ModuleId::Alarm: {
            uint32_t activeMask = 0U;
            uint32_t resettableMask = 0U;
            uint32_t conditionMask = 0U;
            loadAlarmMasks_(activeMask, resettableMask, conditionMask);
            out.available = true;
            out.wireType = RuntimeUiWireType::UInt32;
            if (valueId == 1U) out.u32Value = activeMask;
            else if (valueId == 2U) out.u32Value = resettableMask;
            else if (valueId == 3U) out.u32Value = conditionMask;
            else return false;
            return true;
        }

        case ModuleId::PoolLogic: {
            PoolOperatingConfiguration config;
            if (valueId != 1U || !poolConfigurationSvc_ || !poolConfigurationSvc_->getOperatingConfiguration ||
                !poolConfigurationSvc_->getOperatingConfiguration(poolConfigurationSvc_->ctx, &config)) return false;
            out.available = true;
            out.wireType = RuntimeUiWireType::Enum;
            out.u32Value = uint8_t(config.operatingMode);
            return true;
        }

        case ModuleId::Io:

            switch (valueId) {
                case 1: return readIoValue_(ioIdFromSlot(analogInputSlot(4)), out);
                case 2: return readIoValue_(ioIdFromSlot(analogInputSlot(5)), out);
                case 3: return readIoValue_(ioIdFromSlot(analogInputSlot(1)), out);
                case 4: return readIoValue_(ioIdFromSlot(analogInputSlot(0)), out);
                case 5: return readIoValue_(ioIdFromSlot(digitalInputSlot(PoolInputSlots::WaterMeter)), out);
                case 6: return readIoValue_(ioIdFromSlot(analogInputSlot(2)), out);
                case 7: return readIoBackendValue_(IO_BACKEND_BMP280, 0U, out);
                case 8: return readIoBackendValue_(IO_BACKEND_BME680, 0U, out);
                case 9: return readIoBackendValue_(IO_BACKEND_BMP280, 1U, out);
                case 10: return readIoBackendValue_(IO_BACKEND_SHT40, 0U, out);
                case 11: return readIoBackendValue_(IO_BACKEND_SHT40, 1U, out);
                case 12: return readIoBackendValue_(IO_BACKEND_BME680, 1U, out);
                case 13: return readIoBackendValue_(IO_BACKEND_BME680, 2U, out);
                case 14: return readIoBackendValue_(IO_BACKEND_BME680, 3U, out);
                default: return false;
            }

        case ModuleId::Wifi:
            if (!ds) return false;
            if (valueId == 1U) {
                out.available = true;
                out.wireType = RuntimeUiWireType::Bool;
                out.boolValue = wifiReady(*ds);
                return true;
            }
            if (valueId == 2U) {
                out.available = true;
                out.wireType = RuntimeUiWireType::String;
                formatIp_(out.stringValue, sizeof(out.stringValue));
                return true;
            }
            if (valueId == 3U) {
                if (!WiFi.isConnected()) return false;
                out.available = true;
                out.wireType = RuntimeUiWireType::Int32;
                out.i32Value = (int32_t)WiFi.RSSI();
                return true;
            }
            return false;

        case ModuleId::Mqtt:
            if (!ds) return false;
            if (valueId == 1U) {
                out.available = true;
                out.wireType = RuntimeUiWireType::Bool;
                out.boolValue = mqttReady(*ds);
                return true;
            }
            if (valueId == 3U) {
                out.available = true;
                out.wireType = RuntimeUiWireType::UInt32;
                out.u32Value = mqttRxDrop(*ds);
                return true;
            }
            if (valueId == 4U) {
                out.available = true;
                out.wireType = RuntimeUiWireType::UInt32;
                out.u32Value = mqttParseFail(*ds);
                return true;
            }
            if (valueId == 5U) {
                out.available = true;
                out.wireType = RuntimeUiWireType::UInt32;
                out.u32Value = mqttHandlerFail(*ds);
                return true;
            }
            if (valueId == 6U) {
                out.available = true;
                out.wireType = RuntimeUiWireType::UInt32;
                out.u32Value = mqttOversizeDrop(*ds);
                return true;
            }
            return false;

        case ModuleId::System: {
            SystemStatsSnapshot snap{};
            SystemStats::collect(snap);
            if (valueId == 1U) {
                out.available = true;
                out.wireType = RuntimeUiWireType::String;
                snprintf(out.stringValue, sizeof(out.stringValue), "%s", FirmwareVersion::Full);
                return true;
            }
            out.available = true;
            out.wireType = RuntimeUiWireType::UInt32;
            if (valueId == 2U) {
                out.u32Value = (uint32_t)snap.uptimeMs;
                return true;
            }
            if (valueId == 3U) {
                out.u32Value = snap.heap.freeBytes;
                return true;
            }
            if (valueId == 4U) {
                out.u32Value = snap.heap.minFreeBytes;
                return true;
            }
            return false;
        }

        case ModuleId::PoolDevice: {
            if (!ds) return false;
            uint8_t deviceSlot = 0xFFU;
            if (valueId == 1U) deviceSlot = PoolIds::DeviceFiltrationPump;
            else if (valueId == 2U) deviceSlot = PoolIds::DevicePhPump;
            else if (valueId == 3U) deviceSlot = PoolIds::DeviceChlorinePump;
            else if (valueId == 4U) deviceSlot = PoolIds::DeviceRobot;
            else return false;

            PoolDeviceRuntimeStateEntry state{};
            if (!poolDeviceRuntimeState(*ds, deviceSlot, state)) return false;
            out.available = true;
            out.wireType = RuntimeUiWireType::Bool;
            out.boolValue = state.actualOn;
            return true;
        }

        default:
            return false;
    }
}

void TFTModuleS3::readOverviewStatus_(OverviewStatusRenderState& state) const
{
    state = OverviewStatusRenderState{};
    if (!poolConfigurationSvc_) return;

    PoolOperatingConfiguration config{};
    if (poolConfigurationSvc_->getOperatingConfiguration &&
        poolConfigurationSvc_->getOperatingConfiguration(poolConfigurationSvc_->ctx, &config) &&
        config.available && poolModeValid((uint8_t)config.operatingMode) &&
        poolPhCorrectionValid((uint8_t)config.phCorrection)) {
        state.operatingAvailable = true;
        state.operatingMode = config.operatingMode;
        state.phCorrection = config.phCorrection;
    }
    PoolCharacteristics characteristics{};
    if (poolConfigurationSvc_->getCharacteristics &&
        poolConfigurationSvc_->getCharacteristics(poolConfigurationSvc_->ctx, &characteristics) &&
        characteristics.available &&
        (uint8_t)characteristics.disinfectionMethod <= (uint8_t)PoolDisinfectionMethod::Disabled) {
        state.treatmentAvailable = true;
        state.treatment = characteristics.disinfectionMethod;
    }
}

bool TFTModuleS3::readIoValue_(IoId ioId, char* out, size_t outLen) const
{
    if (!out || outLen == 0U) return false;
    RuntimeValue value{};
    if (!readIoValue_(ioId, value)) return false;
    char unit[8] = {0};
    formatRuntimeValue_(0U, value, out, outLen, unit, sizeof(unit));
    return out[0] != '\0';
}

bool TFTModuleS3::readIoValue_(IoId ioId, RuntimeValue& out) const
{
    if (!ioSvc_ || !ioSvc_->readValue) return false;
    IoValue value{};
    const IoStatus st = ioSvc_->readValue(ioSvc_->ctx, ioId, &value);
    if (st != IO_OK || !value.valid) return false;
    out.available = true;
    if (value.type == IO_VAL_BOOL) {
        out.wireType = RuntimeUiWireType::Bool;
        out.boolValue = value.v.b != 0U;
        return true;
    }
    if (value.type == IO_VAL_INT32) {
        out.wireType = RuntimeUiWireType::Int32;
        out.i32Value = value.v.i32;
        return true;
    }
    out.wireType = RuntimeUiWireType::Float32;
    out.f32Value = value.v.f;
    return true;
}

bool TFTModuleS3::readIoBackendValue_(uint8_t backend, uint8_t channel, char* out, size_t outLen) const
{
    if (!out || outLen == 0U) return false;
    RuntimeValue value{};
    if (!readIoBackendValue_(backend, channel, value)) return false;
    char unit[8] = {0};
    formatRuntimeValue_(0U, value, out, outLen, unit, sizeof(unit));
    return out[0] != '\0';
}

bool TFTModuleS3::readIoBackendValue_(uint8_t backend, uint8_t channel, RuntimeValue& out) const
{
    if (!ioSvc_ || !ioSvc_->count || !ioSvc_->idAt || !ioSvc_->meta) return false;
    const uint8_t count = ioSvc_->count(ioSvc_->ctx);
    for (uint8_t i = 0; i < count; ++i) {
        IoId id = IO_ID_INVALID;
        if (ioSvc_->idAt(ioSvc_->ctx, i, &id) != IO_OK) continue;
        IoEndpointMeta meta{};
        if (ioSvc_->meta(ioSvc_->ctx, id, &meta) != IO_OK) continue;
        if (meta.backend == backend && meta.channel == channel) {
            return readIoValue_(id, out);
        }
    }
    return false;
}



bool TFTModuleS3::readRuntimeUnit_(RuntimeUiId runtimeId, char* out, size_t outLen) const
{
    if (out && outLen > 0U) out[0] = '\0';
    if (!out || outLen == 0U) return false;

    const DataStore* ds = dsSvc_ ? dsSvc_->store : nullptr;
    if (ds && runtimeDataValueId(runtimeId) != VALUE_INVALID) {
        ValueSnapshot snapshot{}; ValueMetadata metadata{};
        if (!ds->values.read(runtimeDataValueId(runtimeId), snapshot, &metadata)) return false;
        snprintf(out, outLen, "%s", metadata.displayUnit);
        return true;
    }


    const RuntimeUiManifestItem* item = findRuntimeUiManifestItem(runtimeId);
    if (item && item->unit && item->unit[0] != '\0') {
        snprintf(out, outLen, "%s", item->unit);
        return true;
    }
    return false;
}

bool TFTModuleS3::readRuntimePrecision_(RuntimeUiId runtimeId, int8_t& out) const
{
    out = VALUE_PRECISION_NONE;
    const DataStore* ds = dsSvc_ ? dsSvc_->store : nullptr;
    if (ds && runtimeDataValueId(runtimeId) != VALUE_INVALID) {
        ValueSnapshot snapshot{}; ValueMetadata metadata{};
        if (!ds->values.read(runtimeDataValueId(runtimeId), snapshot, &metadata)) return false;
        out = metadata.precision;
        return out >= 0;
    }
    return false;
}

uint8_t TFTModuleS3::runtimeDecimals_(RuntimeUiId runtimeId, RuntimeUiWireType wireType, const char* unit) const
{
    if (wireType != RuntimeUiWireType::Float32) return 0U;
    if (runtimeId == makeRuntimeUiId(ModuleId::Io, 3)) return 2U;
    if (runtimeId == makeRuntimeUiId(ModuleId::Io, 6)) return 2U;
    if (unit && strcmp(unit, "mV") == 0) return 0U;
    return 1U;
}

void TFTModuleS3::formatRuntimeValue_(RuntimeUiId runtimeId,
                                      const RuntimeValue& value,
                                      char* valueOut,
                                      size_t valueOutLen,
                                      char* unitOut,
                                      size_t unitOutLen) const
{
    if (valueOut && valueOutLen > 0U) valueOut[0] = '\0';
    if (unitOut && unitOutLen > 0U) unitOut[0] = '\0';
    if (!valueOut || valueOutLen == 0U || !value.available) return;

    char unit[UnitTextCapacity] = {0};
    readRuntimeUnit_(runtimeId, unit, sizeof(unit));

    switch (value.wireType) {
        case RuntimeUiWireType::Bool:
            snprintf(valueOut, valueOutLen, "%s", value.boolValue ? "ON" : "OFF");
            return;
        case RuntimeUiWireType::Int32:
            snprintf(valueOut, valueOutLen, "%ld", (long)value.i32Value);
            break;
        case RuntimeUiWireType::UInt32:
        case RuntimeUiWireType::Enum:
            snprintf(valueOut, valueOutLen, "%lu", (unsigned long)value.u32Value);
            break;
        case RuntimeUiWireType::Float64: {
            int8_t precision = VALUE_PRECISION_NONE;
            readRuntimePrecision_(runtimeId, precision);
            (void)formatValueNumber(valueOut, valueOutLen, value.f64Value, precision);
            break;
        }
        case RuntimeUiWireType::Float32: {
            uint8_t decimals = runtimeDecimals_(runtimeId, value.wireType, unit);
            int8_t derivedPrecision = VALUE_PRECISION_NONE;
            if (readRuntimePrecision_(runtimeId, derivedPrecision)) decimals = (uint8_t)derivedPrecision;
            if (decimals > 0U) {
                snprintf(valueOut, valueOutLen, "%.*f", (int)decimals, (double)value.f32Value);
                trimTrailingZeros_(valueOut);
            } else {
                snprintf(valueOut, valueOutLen, "%ld", lroundf(value.f32Value));
            }
            break;
        }
        case RuntimeUiWireType::String:
            snprintf(valueOut, valueOutLen, "%s", value.stringValue);
            break;
        case RuntimeUiWireType::NotFound:
        case RuntimeUiWireType::Unavailable:
        default:
            return;
    }

    if (unitOut && unitOutLen > 0U && unit[0] != '\0') {
        snprintf(unitOut, unitOutLen, "%s", unit);
    }
}

void TFTModuleS3::slotLabel_(uint8_t slot, char* out, size_t outLen) const
{
    if (!out || outLen == 0U) return;
    out[0] = '\0';
    if (slot >= DashboardSlotCount) return;
    const DashboardSlotConfig& cfg = uiStorage_->dashboardCfg[slot];
    if (cfg.label[0] != '\0') {
        snprintf(out, outLen, "%s", cfg.label);
        return;
    }

    const ValueId dataId = runtimeDataValueId(cfg.runtimeUiId);
    if (dataId != VALUE_INVALID && dsSvc_ && dsSvc_->store) {
        ValueSnapshot snapshot{}; ValueMetadata metadata{};
        dsSvc_->store->values.read(dataId, snapshot, &metadata);
        snprintf(out, outLen, "%s", dataValueLabel(dataId, metadata));
        return;
    }
    const uint8_t valueId = runtimeUiValueId(cfg.runtimeUiId);
    if ((ModuleId)runtimeUiModuleId(cfg.runtimeUiId) == ModuleId::Io &&
        valueId >= IO_RUNTIME_UI_DERIVED_BASE &&
        valueId < IO_RUNTIME_UI_DERIVED_BASE + ValueIds::DerivedCapacity &&
        ioReadDerivedValueName(cfgStore_, (uint8_t)(valueId - IO_RUNTIME_UI_DERIVED_BASE), out, outLen)) {
        return;
    }

    const RuntimeUiManifestItem* item = findRuntimeUiManifestItem(cfg.runtimeUiId);
    const char* key = (item && item->key) ? item->key : "";
    const char* src = strrchr(key, '.');
    src = src ? (src + 1) : key;
    if (!src || src[0] == '\0') {
        snprintf(out, outLen, "Slot %u", (unsigned)slot);
        return;
    }

    bool upperNext = true;
    size_t j = 0U;
    for (size_t i = 0U; src[i] != '\0' && (j + 1U) < outLen; ++i) {
        char ch = src[i];
        if (ch == '_' || ch == '-') {
            out[j++] = ' ';
            upperNext = true;
            continue;
        }
        if (upperNext && ch >= 'a' && ch <= 'z') ch = (char)(ch - ('a' - 'A'));
        out[j++] = ch;
        upperNext = false;
    }
    out[j] = '\0';
}

uint16_t TFTModuleS3::dashboardColor_(uint8_t colorId, uint8_t slot) const
{
    for (size_t i = 0; i < (sizeof(kDashboardColorPresets) / sizeof(kDashboardColorPresets[0])); ++i) {
        if (kDashboardColorPresets[i].id == colorId) return dashboardDarkTint_(kDashboardColorPresets[i].rgb565);
    }
    if (slot < DashboardSlotCount) {
        const uint8_t fallbackId = kDashboardDefaultColorIds[slot];
        for (size_t i = 0; i < (sizeof(kDashboardColorPresets) / sizeof(kDashboardColorPresets[0])); ++i) {
            if (kDashboardColorPresets[i].id == fallbackId) return dashboardDarkTint_(kDashboardColorPresets[i].rgb565);
        }
    }
    return kColorCardBg;
}

const char* TFTModuleS3::alarmIdLabel_(uint16_t alarmId) const
{
    switch ((AlarmId)alarmId) {
        case AlarmId::PoolPsiLow: return "PSI bas";
        case AlarmId::PoolPsiHigh: return "PSI haut";
        case AlarmId::PoolPhTankLow: return "pH vide";
        case AlarmId::PoolChlorineTankLow: return "Chlore vide";
        case AlarmId::PoolPhPumpMaxUptime: return "pH uptime";
        case AlarmId::PoolChlorinePumpMaxUptime: return "ORP uptime";
        case AlarmId::PoolWaterLevelLow: return "Eau basse";
        case AlarmId::None:
        default: return "Alarme";
    }
}

bool TFTModuleS3::readAlarmRuntimeState_(uint16_t alarmId, AlarmSlotRenderState& state) const
{
    if (!alarmSvc_ || !alarmSvc_->buildAlarmState || alarmId == 0U) return false;

    char stateJson[144] = {0};
    if (!alarmSvc_->buildAlarmState(alarmSvc_->ctx, (AlarmId)alarmId, stateJson, sizeof(stateJson))) return false;

    StaticJsonDocument<192> doc;
    if (deserializeJson(doc, stateJson)) return false;
    state.available = true;
    state.latched = (doc["a"] | 0U) != 0U;
    state.resettable = (doc["r"] | 0U) != 0U;
    const uint8_t condition = doc["c"] | 2U;
    state.conditionKnown = condition != (uint8_t)AlarmCondState::Unknown;
    state.conditionTrue = condition == (uint8_t)AlarmCondState::True;

    const uint8_t alarmSlot = doc["slot"] | 255U;
    const uint32_t mask = (alarmSlot < 32U) ? (1UL << alarmSlot) : 0U;
    if (state.label[0] == '\0' && mask != 0U) {
        const char* runtimeLabel = runtimeUiAlarmActiveFlagLabel(mask, "fr");
        if (runtimeLabel && runtimeLabel[0] != '\0') {
            snprintf(state.label, sizeof(state.label), "%s", runtimeLabel);
        }
    }
    return true;
}

void TFTModuleS3::loadAlarmMasks_(uint32_t& activeMask, uint32_t& resettableMask, uint32_t& conditionMask) const
{
    activeMask = 0U;
    resettableMask = 0U;
    conditionMask = 0U;
    if (!alarmSvc_ || !alarmSvc_->listIds || !alarmSvc_->buildAlarmState) return;

    AlarmId ids[Limits::Alarm::MaxAlarms] = {};
    const uint8_t count = alarmSvc_->listIds(alarmSvc_->ctx, ids, (uint8_t)Limits::Alarm::MaxAlarms);
    for (uint8_t i = 0U; i < count; ++i) {
        char stateJson[144] = {0};
        if (!alarmSvc_->buildAlarmState(alarmSvc_->ctx, ids[i], stateJson, sizeof(stateJson))) continue;

        StaticJsonDocument<192> doc;
        if (deserializeJson(doc, stateJson)) continue;
        const uint8_t slot = doc["slot"] | 255U;
        if (slot >= 32U) continue;
        const uint32_t bit = (1UL << slot);
        if ((doc["a"] | 0U) != 0U) activeMask |= bit;
        if ((doc["r"] | 0U) != 0U) resettableMask |= bit;
        if ((doc["c"] | 0U) == 1U) conditionMask |= bit;
    }
}

uint32_t TFTModuleS3::currentPageCycle_() const
{
    if (!splashHeld_) return 0U;
    return (uint32_t)((millis() - pageCycleStartMs_) / kPageRotateMs);
}

bool TFTModuleS3::dashboardSlotStateEquals_(const DashboardSlotRenderState& a, const DashboardSlotRenderState& b)
{
    return a.valid == b.valid &&
           a.enabled == b.enabled &&
           a.available == b.available &&
           a.cardBg == b.cardBg &&
           strncmp(a.label, b.label, sizeof(a.label)) == 0 &&
           strncmp(a.value, b.value, sizeof(a.value)) == 0 &&
           strncmp(a.unit, b.unit, sizeof(a.unit)) == 0;
}

bool TFTModuleS3::alarmSlotStateEquals_(const AlarmSlotRenderState& a, const AlarmSlotRenderState& b)
{
    return a.valid == b.valid &&
           a.enabled == b.enabled &&
           a.available == b.available &&
           a.latched == b.latched &&
           a.resettable == b.resettable &&
           a.conditionTrue == b.conditionTrue &&
           a.conditionKnown == b.conditionKnown &&
           a.cardBg == b.cardBg &&
           a.alarmId == b.alarmId &&
           strncmp(a.label, b.label, sizeof(a.label)) == 0;
}

bool TFTModuleS3::runtimeUiAllowed_(RuntimeUiId runtimeId)
{
    if (!isValidRuntimeUiId(runtimeId)) return false;
    return findRuntimeUiManifestItem(runtimeId) != nullptr;
}

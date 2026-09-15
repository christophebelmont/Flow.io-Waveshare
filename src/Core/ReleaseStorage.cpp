/**
 * @file ReleaseStorage.cpp
 * @brief Runtime selection and validation of A/B release filesystems.
 */

#include "Core/ReleaseStorage.h"
#include "Core/FirmwareVersion.h"

#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace {

fs::SPIFFSFS gRuntimeFilesystem;
ReleaseSlot gRunningSlot = ReleaseSlot::A;
bool gReleaseReady = false;
bool gRuntimeReady = false;

ReleaseSlot slotFromPartition(const esp_partition_t* partition)
{
    if (partition && partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        return ReleaseSlot::B;
    }
    return ReleaseSlot::A;
}

bool runningApplicationIsPending(const esp_partition_t* running)
{
    if (!running) return false;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    return esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

}  // namespace

namespace ReleaseStorage {

bool beginReleaseFilesystem()
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    gRunningSlot = slotFromPartition(running);
    gReleaseReady = SPIFFS.begin(false, "/spiffs", 10, filesystemLabel(gRunningSlot));
    if (!gReleaseReady && runningApplicationIsPending(running)) {
        // setup cannot reach the normal startup-complete confirmation without
        // its release filesystem, so reject the candidate immediately.
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    return gReleaseReady;
}

bool beginRuntimeFilesystem()
{
    // Runtime data is not part of a release. It may be formatted on the first
    // USB installation, unlike either release filesystem slot.
    gRuntimeReady = gRuntimeFilesystem.begin(true, "/runtime", 4, kRuntimeLabel);
    return gRuntimeReady;
}

ReleaseSlot runningSlot()
{
    return gRunningSlot;
}

ReleaseSlot inactiveSlot()
{
    return gRunningSlot == ReleaseSlot::A ? ReleaseSlot::B : ReleaseSlot::A;
}

const char* filesystemLabel(ReleaseSlot slot)
{
    return slot == ReleaseSlot::A ? kSlotALabel : kSlotBLabel;
}

const char* applicationLabel(ReleaseSlot slot)
{
    return slot == ReleaseSlot::A ? "app0" : "app1";
}

bool releaseReady()
{
    return gReleaseReady;
}

bool runtimeReady()
{
    return gRuntimeReady;
}

fs::FS& releaseFilesystem()
{
    return SPIFFS;
}

fs::FS& runtimeFilesystem()
{
    return gRuntimeFilesystem;
}

bool validateReleaseFilesystem()
{
    if (!gReleaseReady) return false;
    return validateReleaseFilesystem(SPIFFS, FirmwareVersion::Core, "WaveshareESP32S3");
}

bool validateReleaseFilesystem(fs::FS& filesystem,
                               const char* expectedVersion,
                               const char* expectedHardware)
{
    if (!expectedVersion || !expectedHardware) return false;
    static constexpr const char* kRequiredAssets[] = {
        "/release.json",
        "/webinterface/index.html.gz",
        "/webinterface/app-core.js.gz",
        "/webinterface/app.js.gz",
        "/webinterface/app-core.css.gz"
    };
    for (const char* path : kRequiredAssets) {
        if (!filesystem.exists(path)) return false;
    }
    File descriptor = filesystem.open("/release.json", FILE_READ);
    StaticJsonDocument<256> doc;
    const bool valid = descriptor &&
                       deserializeJson(doc, descriptor) == DeserializationError::Ok &&
                       (doc["format"] | 0U) == 1U &&
                       strcmp(doc["product"] | "", "Flow.IO") == 0 &&
                       strcmp(doc["hardware"] | "", expectedHardware) == 0 &&
                       strcmp(doc["version"] | "", expectedVersion) == 0;
    if (descriptor) descriptor.close();
    return valid;
}

bool confirmPendingApplication()
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return false;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return true;
    if (!validateReleaseFilesystem()) {
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
        return false;
    }
    return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

}  // namespace ReleaseStorage

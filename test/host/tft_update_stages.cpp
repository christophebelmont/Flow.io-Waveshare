// g++ -std=c++17 -Wall -Wextra -Werror -pedantic -Isrc test/host/tft_update_stages.cpp -o /tmp/tft_steps && /tmp/tft_steps
#include "Modules/TFTModuleS3/TftUpdateStages.h"
#include <cassert>
#include <cstring>

int main() {
    using tft_update::steps;
    using tft_update::isEnglish;
    assert(isEnglish("en"));
    assert(isEnglish("en-GB"));
    assert(!isEnglish("fr"));
    const auto manifest = steps(false, "manifest", false);
    assert(std::strcmp(manifest.current, "Lecture du manifeste") == 0);
    assert(manifest.previous == nullptr);
    const auto fsDl = steps(false, "spiffs_download", true);
    assert(std::strcmp(fsDl.current, "Downloading filesystem") == 0);
    assert(fsDl.index < fsDl.count);
    const auto fsStream = steps(false, "spiffs_stream", false);
    assert(std::strcmp(fsStream.current, "Ecriture SPIFFS") == 0);
    const auto fwStream = steps(false, "firmware_stream", true);
    assert(std::strcmp(fwStream.current, "Writing firmware") == 0);
    const auto activate = steps(false, "activate", false);
    assert(std::strcmp(activate.current, "Activation A/B") == 0);
    const auto zipPrep = steps(false, "zip_manifest", false, true, false, true);
    assert(zipPrep.index == 0U);
    assert(std::strcmp(zipPrep.current, "Preparation du ZIP") == 0);
    const auto zipReceive = steps(false, "zip_spiffs_receive", false);
    assert(std::strcmp(zipReceive.current, "Reception SPIFFS en RAM") == 0);
    assert(zipReceive.index == 2U);
    const auto zipHashEn = steps(false, "zip_spiffs_ram_verify", true);
    assert(std::strcmp(zipHashEn.current, "Checking SPIFFS in RAM") == 0);
    const auto zipFlash = steps(false, "zip_spiffs_flash", false);
    assert(std::strcmp(zipFlash.current, "Ecriture SPIFFS Flash") == 0);
    const auto zipReadback = steps(false, "zip_firmware_readback", true);
    assert(std::strcmp(zipReadback.current, "Verifying firmware Flash") == 0);
    const auto zipVerified = steps(false, "zip_firmware_verified", true);
    assert(zipVerified.index == zipReadback.index);
    const auto zipActivate = steps(false, "zip_activate", false);
    assert(zipActivate.index == 12U);
    const auto zipRestart = steps(false, "", true, false, true, true);
    assert(std::strcmp(zipRestart.current, "Restarting") == 0);
    assert(zipRestart.index == 13U);
    const auto nextion = steps(true, "nextion_upload", false);
    assert(std::strcmp(nextion.current, "Transfert vers Nextion") == 0);
    const auto nextionEn = steps(true, "nextion_ack", true);
    assert(std::strcmp(nextionEn.current, "Final Nextion ACK") == 0);
    const auto queued = steps(true, "nextion_upload", true, true);
    assert(std::strcmp(queued.current, "Preparing update") == 0);
    const auto restart = steps(false, "", false, false, true);
    assert(std::strcmp(restart.current, "Redemarrage") == 0);
    const auto local = steps(false, "", true);
    assert(std::strcmp(local.current, "Installing update") == 0);
    assert(std::strcmp(tft_update::failureReason("Nextion PSRAM SHA-256 mismatch", false),
                       "Echec controle integrite") == 0);
    assert(std::strcmp(tft_update::failureReason("network not connected", true),
                       "Network/download failed") == 0);
    return 0;
}

#pragma once
// Local, allocation-free labels for the 320x240 ST7789 update screen.
// Deliberately ASCII: the embedded GFX font does not reliably render UTF-8 accents.
// All labels are based on real FirmwareUpdateModule::setOtaStage_ tokens.
#include <stdint.h>
#include <string.h>

namespace tft_update {

struct Stage {
    const char* id;
    const char* fr;
    const char* en;
};

struct Text {
    const char* previous;
    const char* current;
    const char* next;
    uint8_t index;
    uint8_t count;
};

inline bool isEnglish(const char* language) {
    return language && (language[0] == 'e' || language[0] == 'E') &&
           (language[1] == 'n' || language[1] == 'N');
}

inline const char* local(const Stage& stage, bool english) {
    return english ? stage.en : stage.fr;
}

inline Text steps(bool nextion, const char* phase, bool english,
                  bool queued = false, bool rebooting = false) {
    static constexpr Stage ota[] = {
        {"manifest", "Lecture du manifeste", "Reading manifest"},
        {"spiffs_download", "Telechargement SPIFFS", "Downloading filesystem"},
        {"spiffs_ram_verify", "Controle SHA-256 SPIFFS", "Checking filesystem hash"},
        {"spiffs_prepare", "Preparation SPIFFS", "Preparing filesystem"},
        {"spiffs_write", "Ecriture SPIFFS", "Writing filesystem"},
        {"spiffs_verify", "Validation SPIFFS", "Verifying filesystem"},
        {"firmware_download", "Telech. du firmware", "Downloading firmware"},
        {"firmware_ram_verify", "Controle SHA-256 firmware", "Checking firmware hash"},
        {"firmware_prepare", "Preparation du firmware", "Preparing firmware"},
        {"firmware_write", "Ecriture du firmware", "Writing firmware"},
        {"firmware_verify", "Validation du firmware", "Verifying firmware"},
        {"activate", "Activation A/B", "Activating A/B slots"},
        {"reboot", "Redemarrage", "Restarting"}
    };
    static constexpr Stage hmi[] = {
        {"queued", "Preparation de la MAJ", "Preparing update"},
        {"nextion_download", "Telech. du fichier TFT", "Downloading TFT file"},
        {"nextion_verify", "Verification SHA-256", "Verifying SHA-256"},
        {"nextion_uart", "Preparation UART", "Preparing UART"},
        {"nextion_upload", "Transfert vers Nextion", "Uploading to Nextion"},
        {"nextion_ack", "Accuse final Nextion", "Final Nextion ACK"},
        {"reboot", "Redemarrage", "Restarting"}
    };
    const Stage* list = nextion ? hmi : ota;
    const uint8_t count = nextion ?
        static_cast<uint8_t>(sizeof(hmi) / sizeof(hmi[0])) :
        static_cast<uint8_t>(sizeof(ota) / sizeof(ota[0]));
    uint8_t idx = 0U;
    if (rebooting) idx = count - 1U;
    else if (!queued && phase && phase[0]) {
        // The OTA can stream directly instead of replaying a PSRAM image.
        const char* canonical = phase;
        if (strcmp(phase, "spiffs_stream") == 0) canonical = "spiffs_write";
        if (strcmp(phase, "firmware_stream") == 0) canonical = "firmware_write";
        for (uint8_t i = 0; i < count; ++i) {
            if (strcmp(canonical, list[i].id) == 0) {
                idx = i;
                break;
            }
        }
    }
    // Local ZIP upload has no per-image OTA stage on some paths.
    const bool unrecognized = !queued && !rebooting && (!phase || !phase[0]);
    const char* current = unrecognized ? (english ? "Installing update" : "Installation en cours")
                                       : local(list[idx], english);
    return {
        idx > 0U ? local(list[idx-1U], english) : nullptr,
        current,
        idx + 1U < count ? local(list[idx+1U], english) : nullptr,
        idx, count
    };
}

inline const char* failureReason(const char* message, bool english) {
    if (!message) return english ? "Check event logs" : "Consulter les journaux";
    if (strstr(message, "SHA") || strstr(message, "checksum") || strstr(message, "digest"))
        return english ? "Integrity check failed" : "Echec controle integrite";
    if (strstr(message, "HTTP") || strstr(message, "download") || strstr(message, "network"))
        return english ? "Network/download failed" : "Echec reseau/telecharg.";
    if (strstr(message, "UART") || strstr(message, "upload") || strstr(message, "ACK"))
        return english ? "Display transfer failed" : "Echec transfert ecran";
    if (strstr(message, "PSRAM") || strstr(message, "allocation"))
        return english ? "Not enough memory" : "Memoire insuffisante";
    return english ? "Check event logs" : "Consulter les journaux";
}
} // namespace tft_update

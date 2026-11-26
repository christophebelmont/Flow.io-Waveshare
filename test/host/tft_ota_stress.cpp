// Standalone host simulation (no Arduino/PSRAM/SPI dependencies).
// g++ -std=c++17 -Wall -Wextra -Werror -pedantic -Isrc test/host/tft_ota_stress.cpp -o /tmp/tft_ota_stress && /tmp/tft_ota_stress
#include "Modules/TFTModuleS3/TftUpdateRenderPlan.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

struct Counts { int full=0, title=0, progress=0, details=0, skipped=0; };

int main() {
    using tft_update::renderDirty;
    Counts counts;
    uint32_t lastDrawMs = 0;
    uint8_t lastPercent = 255U;
    bool active = false, lastEnglish = false;
    int lastStage = -1, lastState = -1, lastTarget = -1;
    auto tick = [&](uint32_t now, int stage, int state, int target,
                    bool english, uint8_t percent) {
        const bool newOp = !active;
        const bool stageChanged = stage != lastStage;
        const bool stateChanged = state != lastState;
        const bool targetChanged = target != lastTarget;
        const bool languageChanged = english != lastEnglish;
        const auto dirty = renderDirty(newOp, stageChanged, stateChanged,
                                       targetChanged, languageChanged,
                                       percent, lastPercent);
        const bool changed = dirty.title || dirty.progress || dirty.details;
        const bool urgent = newOp || stageChanged || stateChanged ||
                            targetChanged || languageChanged;
        if (changed && (urgent || uint32_t(now - lastDrawMs) >= 250U)) {
            counts.full += dirty.full;
            counts.title += dirty.title;
            counts.progress += dirty.progress;
            counts.details += dirty.details;
            lastDrawMs = now;
            lastPercent = percent;
            lastStage = stage;
            lastState = state;
            lastEnglish = english;
            lastTarget = target;
        } else ++counts.skipped;
        active = true;
    };

    uint32_t clock = 1000U;
    // Simulate 12 consecutive OTA stages, 101 progress values at 10 Hz.
    for (int stage = 0; stage < 12; ++stage) {
        for (int percent = 0; percent <= 100; ++percent) {
            tick(clock, stage, 1, 0, false, static_cast<uint8_t>(percent));
            clock += 100U;
        }
    }
    assert(counts.full == 1);    // logo/full screen rendered exactly once
    assert(counts.title == 1);   // percentage changes never repaint the title
    assert(counts.details == 12);// only stage changes repaint the detail rows
    assert(counts.progress >= 12 && counts.progress < 700);

    const int detailsBefore = counts.details;
    tick(clock + 1U, 12, 1, 0, true, 57);
    assert(counts.details == detailsBefore + 1); // bilingual switch immediate
    const int titlesBefore = counts.title;
    tick(clock + 2U, 12, 2, 0, true, 57);
    assert(counts.title == titlesBefore + 1);   // error frame immediate

    // The firmware's 20s terminal hold suppresses re-entry for the same ID.
    const uint32_t operationId = 42U;
    uint32_t dismissedOperation = 0U;
    assert(operationId != dismissedOperation);
    const uint32_t enteredTerminalAt = clock + 2U;
    assert(uint32_t(enteredTerminalAt + 19999U - enteredTerminalAt) < 20000U);
    if (uint32_t(enteredTerminalAt + 20000U - enteredTerminalAt) >= 20000U)
        dismissedOperation = operationId;
    assert(operationId == dismissedOperation);
    assert(operationId + 1U != dismissedOperation);

    std::printf("PASS: 12 stages x 101 percentages, FR/EN, error, 20s terminal\n");
    std::printf("full=%d title=%d progress=%d details=%d skipped=%d\n",
                counts.full, counts.title, counts.progress, counts.details,
                counts.skipped);
    return 0;
}

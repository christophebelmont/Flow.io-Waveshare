#pragma once
// Hardware-independent dirty-area decision; no SPI or heap needed.
#include <stdint.h>

namespace tft_update {

struct RenderDirty {
    bool full;
    bool title;
    bool progress;
    bool details;
};

inline RenderDirty renderDirty(bool newScreen, bool stageChanged, bool stateChanged,
                               bool targetChanged, bool languageChanged,
                               uint8_t percent, uint8_t previousPercent)
{
    const bool title = newScreen || stateChanged || targetChanged || languageChanged;
    const bool details = title || stageChanged;
    const uint8_t delta = percent >= previousPercent
        ? static_cast<uint8_t>(percent - previousPercent)
        : static_cast<uint8_t>(previousPercent - percent);
    // 1% progress increments accumulate, but stage resets and 100% are visible.
    const bool progress = details || previousPercent > 100U ||
        (percent != previousPercent &&
         (percent == 0U || percent == 100U || delta >= 2U));
    return {newScreen, title, progress, details};
}

} // namespace tft_update

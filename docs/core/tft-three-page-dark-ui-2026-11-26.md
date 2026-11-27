# Flow.io Waveshare TFT — unified dark 320×240 dashboard (experimental)

**Branch:** `experiment/tft-unified-three-pages-20261126`  
**Status:** firmware implementation committed; hardware compilation and visual acceptance pending.

## Scope

- Three rotating pages, **native 320×240** (no virtual high-resolution rendering).
- Shared **32px dark header**: enlarged `flow.io` label, five red-to-green RSSI bars, authentic Flow.io wave derived from existing `FlowIoLogoBitmap.h`, `flow.io` text at left, three page dots at centre, narrow Wi-Fi RSSI bars and the time at right. Only the active page dot changes.
- Page 1: the IP and `http://flowio.local` above three read-only mode cards. Only the **current selected value** is shown for Mode, Correction pH and Traitement.
- Page 2: always **5 rows × 2 columns**; up to ten configured metrics in reading order. Small colorful 16px icon left, **no decorative separator**, label in the available space, **value right-aligned immediately before its unit**. Long labels can split at the first space. Slots, names and units remain configurable.
- Page 3: **4 rows × 2 columns**, preserving the configured alarm list (seven defaults; eighth potentially empty). No vertical icon separators. Each alarm keeps independent latched/condition status; **L above C at the right**. More horizontal room for the label; different needle directions for low/high PSI; colored icons for pH, chlorine, timers and water level.
- Dark backgrounds with cyan outlines and colored 16px vector pictograms. Selected card tint IDs are retained, but converted into subtle dark shades.

## Low-flicker rendering

- No continuous clearing of the full display.
- Full content repaint only for first render, page rotation, backlight reset or changed slot layout; **the static header persists between page rotations**.
- Wi-Fi and clock refresh independently in small dirty rectangles. Minute updates do not repaint Wi-Fi or vice versa.
- A metric **value-only** change clears and redraws only the inner text area; frame, icon and separator remain intact.
- A status-only alarm change redraws **only the two L/C circles**.
- Existing flow-meter 5-second TFT rate limiter remains in place.
- Firmware-update and Nextion progress rendering paths and their logo/OTA stages are not changed.

## Validation

- Geometry checked at native 320×240: left/right/bottom margins **1px**, column and row gaps **2px**.
  - Screen 1: IP/URL tiles **158×41px** at y=33, three status cards **318×53px** at y=76, 131 and 186.
  - Screen 2: ten **158px-wide** cards in 5×2; 39px heights (40px final row), starting at y=35.
  - Screen 3: eight **158×50px** slots in 4×2 starting at y=33; seven alarms enabled by default.
- `scripts/tests/test_tft_overview_selectors.py` checks source invariants and compiles/runs a native host fixture for the overview + geometry when PlatformIO's Adafruit GFX dependency is present.
- Run `python3 -m unittest scripts.tests.test_tft_overview_selectors` and `pio run -e Flowio-waveshare-esp32-s3` on a development host before flashing.
- Physical TFT acceptance should check tiny-label readability, glyphs, Wi-Fi/time alignment, PSI needle direction, both L/C state combinations, page cycling and absence of perceptible redraw flicker.

**Note:** This experiment changes the local firmware TFT interface only. It does not change the Modbus scheduler, MQTT/HA, OTA protocol, SPIFFS, or external Nextion screens.

# Local TFT — OTA / Nextion update progress (25 November 2026)

Implementation branch: `feature/tft-ota-nextion-progress-20261125`
(base: `stephdust/Flow.io-Waveshare:dev` at `1382ffc2`).

## Rendering and priorities

- The local Waveshare ESP32-S3 **ST7789 320×240 landscape TFT** shows a
  dedicated update screen while an operation is queued, downloading,
  writing, rebooting or in its 20-second failure/result window.
- Reuses the **existing 181×86 Flow.io RGB565 logo and 92×26 splash wordmark**
  on their original white background. A dark lower panel provides the
  horizontal cyan progress bar and the primary current-stage text.
- The **active step is prominent**; a green "OK" appears only for a step
  the TFT actually observed transitioning away from, never for an assumed
  PSRAM stage that may be skipped. The next expected step is muted.
- The percentage is **stage-specific** when `otaStage` is available,
  otherwise the already published overall percentage. Thus downloading
  into PSRAM does not look frozen at 0% while bytes are received.
- Messages are selected using `LocaleService::language()` (FR / EN).
  Embedded text is ASCII-only because the deployed Adafruit GFX fonts do not
  provide reliable UTF-8 accent rendering on this screen.
- The TFT wakes its backlight even if PIR auto-sleep was active, and does not
  rotate dashboards while an update is in progress. The previous dashboard
  is fully invalidated and restored once update mode is exited.
- Redraws are capped at **4 Hz** and only made when operation, state,
  stage, phase percentage or language changes; the logo is drawn only
  once on entry. All SPI drawing runs in the existing TFT task on core 1.

## Data architecture

`FirmwareUpdateService::displaySnapshot()` performs a short,
allocation-free copy under the firmware module's existing spinlock.
Unlike the Web status endpoint it does **not** serialize JSON, allocate
an ArduinoJson document, perform an HTTP request, or drive the OTA worker.

Source of stage IDs remains exclusively the existing
`FirmwareUpdateModule::setOtaStage_()` calls:

- **OTA:** manifest, SPIFFS download / SHA verification / prepare /
  flash write / verification, firmware download / SHA verification /
  prepare / write / verification, A/B activation and reboot.
- **Nextion:** download to PSRAM, SHA-256 verification, UART preparation,
  UART transfer, final Nextion ACK, reboot.

The implementation also recognizes OTA direct streaming
(`spiffs_stream`, `firmware_stream`) and local ZIP installation paths.
It reuses the persisted `lastFailure_` stage after an error so the
screen can identify the failing step. Error reasons are classified and
translated for the TFT; exact error messages are preserved unchanged
in existing firmware logs and Web status.

## Non-regression conditions

- No modification to the OTA A/B partitioning, SHA-256 validation,
  PSRAM staging, HTTP retries, Nextion UART transfer, shared RS485
  scheduling, Modbus TCP pause/resume or reboot admission.
- A failed update displays a red failure state for about 20 seconds;
  it does **not** reset the device or attempt an automatic retry.
- A planned firmware reboot may occur too quickly for the final TFT
  "Restarting" frame to be physically visible. Firmware safety is
  intentionally not delayed merely to display an animation.
- Existing disabled TFT configuration is respected.
- Host label tests:
  `g++ -std=c++17 -Wall -Wextra -Werror -pedantic -Isrc test/host/tft_update_stages.cpp -o /tmp/tft_steps && /tmp/tft_steps`.
- Static source regression:
  `node scripts/tests/check_tft_update_progress.mjs`.

**Validation to perform on the ESP32-S3:** PlatformIO build, OTA on
ports 502/503 listening, Nextion transfer with the TFT awake and asleep,
English/FR switching, bad HTTP/SHA/ACK, recovery after failure,
next boot/splash, and backlight wake behavior.

## 25 November follow-up: compact black update screen

- Local OTA/Nextion update UI now uses a **black full-screen background**.
- Existing white-matted brand icon and wordmark are scaled down, colour-keyed
  at draw time, and painted **once per update**, not on every progress tick.
  Icon 96x46, wordmark 69x19; normal boot/wake splash remains unchanged.
- Progress redraw now touches **only** the bar interior and percentage field.
  Text rows update on stage, state, target or language changes; the static logo
  does not move. 1% changes accumulate until 2% or a phase reset/100%.
- Stage changes and terminal success/failure states remain immediately visible.
- Added host-testable rendering-decision rules in TftUpdateRenderPlan.h and
  native C++ simulation cases in test/host/tft_update_render_plan.cpp.
- Real SPI/TFT tests remain needed to measure flicker and text clipping.

## 25 November follow-up: readable stages and a static 3D progress bar

- The right-aligned progress figure displays only `65%`, not `ETAPE 65%`
  / `STEP 65%`; the larger 2x built-in font improves readability.
- OTA/Nextion step rows now follow their natural chronological order:
  last observed completed step on top (green check), **current step** in the
  middle (accent strip, cyan or red), and next step/error detail below (muted
  or red). There is no isolated `OK` between moving rows.
- The progress bar gets a subtle beveled metallic rim, upper reflected
  highlight, a 4-band cyan/green/red vertical fill and a bright leading edge.
  There is **no continuously animated shine**: on each relevant percentage
  change, only the interior and percentage region are repainted.
- Text gets a smaller font if necessary, then an ellipsis before the
  320px boundary, rather than overflowing visibly to the next area.
- No change to actual OTA/Nextion state machine, PSRAM staging, partition
  writes, Modbus TCP or logging.

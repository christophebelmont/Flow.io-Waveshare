# Changelog — Web Dashboard mirrors active TFT S3 sensor slots

**Date:** 2026-11-24  
**Target:** `christophebelmont/Flow.io-Waveshare:dev`  
**Source:** `stephdust/Flow.io-Waveshare:feature/web-dashboard-10-active-tft-sensors-20261124`

## Summary

The Flow.io Web Dashboard sensor section now follows the **10 configurable TFT S3 sensor slots**, displaying only slots explicitly enabled in the TFT S3 configuration.

## Changed

- Extended the `GET /api/runtime/dashboard_slots` sensor response from 8 to **10** entries (`slot00`–`slot09`), using the same saved `tft/s3/sensors/slotXX` settings as the TFT.
- Kept the eight existing sensor defaults; the two newly available slots are **disabled by default**, matching the TFT's opt-in behavior.
- Extended Web Dashboard JavaScript to handle all ten slots, sorted by configured slot index.
- Filtered out disabled sensor slots instead of displaying empty placeholders. An enabled slot with unavailable runtime data remains visible as unavailable.
- Preserved configured sensor labels, colors, values, units and display ranges.
- Added a clear empty state when no TFT sensor slot is enabled.
- Kept the **alarm dashboard limited to eight slots**, with its defaults and rendering unchanged.

## Files changed

- `src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp`
- `data/webinterface/app.js`
- `scripts/tests/test_web_dashboard_tft_sensor_slots.py` (new regression guards)

## Testing and acceptance

- Source-level regression checks cover 10 TFT/web sensor slots, eight unchanged alarms, opt-in defaults for new sensors, active-only filtering and slot order.
- User reported the feature working in their Flow.io test environment on 2026-11-24.
- **Not claimed:** a PlatformIO build or an independently recorded automated end-to-end hardware/browser test in this change report.

## Scope and compatibility

No TFT display rendering changes, HA discovery changes, `famille5` changes or Modbus/RS485 changes are included. Existing TFT S3 sensor configuration is reused; no configuration migration is required.

To verify manually, enable `tft/s3/sensors/slot08` or `slot09`, assign a runtime measurement, and reload the Web Dashboard. The configured sensor should appear. Disabling that slot should remove it from the web list without changing the alarm list.

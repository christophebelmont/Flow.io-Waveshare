# Changelog — TFT S3 five-row metric value font

**Date:** 2026-11-24  
**Target:** `christophebelmont/Flow.io-Waveshare:dev`  
**Source:** `stephdust/Flow.io-Waveshare:fix/tft-five-row-values-font-20261124`

## Summary

Improve readability in the TFT S3 **five-row / nine-to-ten-active-sensor** dashboard layout by slightly reducing the font size of displayed metric **values**.

## Changed

- Replace the `FreeSansBold9pt7b` value font with a new compact **`FreeSansBold8pt7b`** font when the TFT dashboard uses the dense five-row layout, a reduction of approximately 11% in nominal point size.
- Add the ASCII GFX-compatible `FreeSansBold8pt7b` bitmap font as a local header. The bitmap and glyph metadata are stored in flash (`PROGMEM`), with no runtime heap allocations.
- Keep the normal four-row layout's **`FreeSansBold12pt7b`** value font unchanged.
- Leave metric labels, units, colors, layout logic, sensor configuration, alarm displays and screen navigation unchanged.

## Files changed

- `src/Modules/TFTModuleS3/TFTModuleS3.cpp`
- `src/Modules/TFTModuleS3/FreeSansBold8pt7b.h` (new)

## Validation

- The user confirmed the revised font renders satisfactorily on the TFT S3.
- Source-level checks verified the GFX font bitmap structure (95 printable ASCII glyphs), bitmap offsets and checksum, the dense-mode font selection, and the unchanged four-row font selection.
- A separately recorded PlatformIO build, CI check or automated display screenshot comparison is **not claimed**.

## Compatibility and scope

This is a **display-only** adjustment. It does not alter TFT configuration persistence, Web Dashboard, Modbus/RS485, MQTT, Home Assistant, OTA, or the pending `family5` work. No configuration migration is required.

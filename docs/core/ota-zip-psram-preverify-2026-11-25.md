# Secure local ZIP OTA — PSRAM preverify + asynchronous A/B Flash

Date: 2026-11-25. Branch: `feature/zip-psram-preverify-async-flash-20261125`.

## Problem observed

A successful ZIP upload could be followed by a firmware HTTP reset and an OTA
that failed until reboot. Browser log for transaction `1552688753`:
`ERR_CONNECTION_RESET` 4.17 s into firmware upload (HTTP 0, 81,920 bytes
reported sent). A previous Flash initialization took ~3.99 s while AsyncTCP
was synchronously blocked. A subsequent commit/abort could recover, but these
failures were intermittent.

Earlier changes protected the **inactive A/B partitions**, SHA-256 checks,
and delegated Flash operations to fwupdate. However, the local ZIP handler
**still wrote each incoming HTTP chunk directly to Flash** before completing
the file's SHA-256. The remote paired OTA and Nextion preverify their images
in PSRAM; local ZIP did not.

## Final architecture

The browser opens the uncompressed, three-entry ZIP. Flow.io never stores
or decompresses the entire archive on the ESP32.

1. `manifest.json`: validates version, target, sizes and SHA-256.
2. POST `/api/upgrade/filesystem`: allocate **one 1.5 MiB PSRAM buffer**,
   preserving at least **2 MiB** and requiring a sufficiently large contiguous
   region. Copy the SPIFFS HTTP body into PSRAM only; enforce offsets/sizes.
3. `endLocalImage_` verifies the **entire received SHA-256 against the
   manifest BEFORE any inactive partition erase or write**. A mismatch fails
   closed and frees PSRAM. If good, respond **HTTP 202 staged** promptly.
4. Browser GET `/api/upgrade/image/status?transaction_id=...&target=spiffs`
   acknowledges the 202 and starts async Flash after a 250 ms response grace
   period. The GET returns a tiny lock-copied snapshot; it never waits on the
   Flash mutex or mounts SPIFFS. Repeated GET cannot enqueue another writer.
5. The fwupdate task erases/writes the inactive SPIFFS partition using an
   **internal 1024-byte buffer**. Read back all written bytes and recompute
   SHA-256; validate the candidate SPIFFS. Only then report `verified`, free
   PSRAM and permit the next image.
6. Repeat steps 2–5 for `firmware.bin` (about 2.72 MiB). Its Flash writer
   uses `esp_ota_begin(..., OTA_WITH_SEQUENTIAL_WRITES)` then
   `esp_ota_write`, `esp_ota_end`, full-partition SHA-256 readback.
7. POST `/api/upgrade/commit` **only after both snapshots read verified**.
   Confirm A/B boot-slot selection, persist the update receipt and reboot.
   The remote manifest-driven OTA / Nextion workers are not modified.

The SHA-256 checks provide **integrity** against the supplied manifest. This
mechanism does not add digital signature authentication of who created the ZIP.

## Safety & failure handling

- **Insufficient or fragmented PSRAM:** reject without touching an A/B image
  partition. No silent fallback to concurrent HTTP-to-Flash streaming.
- **Interrupted upload / missing first GET:** no Flash; inactive ZIP buffer
  discarded by abort or the existing 120-second inactivity watchdog.
- **Wrong offset, size or SHA:** abort, free PSRAM, do not start Flash.
- **Flash erase/write, `esp_ota_end`, readback SHA or SPIFFS validation
  failure:** mark the image failed, free PSRAM, never commit the new boot slot.
- **Repeated browser GET:** one-shot Flash start, protected by queue/start
  flags. No duplicate writes.
- **Browser refresh during asynchronous Flash:** the worker is independent of
  the HTTP callback and continues safely; an abort can run once it finishes.
- **New OTA/Nextion before ZIP complete:** admission barrier remains closed.
  After an aborted/error ZIP, it opens again without requiring reboot.

All existing `[OTA-ZIP-*]` diagnostics remain. New prefixes:
`[OTA-ZIP-PSRAM] stage_allocated`, `preverify_ok`, `flash_begin`,
`flash_progress`, `flash_readback_sha256`, `flash_verified`. The browser
logs `flash_state` and `flash_verified`.

## Validation before field deployment

Source-contract and host simulations:
```bash
node scripts/tests/check_zip_ab_release.mjs
node scripts/tests/check_zip_psram_preverify_async.mjs
node scripts/tests/check_ota_deep_diagnostics.mjs
node scripts/tests/simulate_local_zip_ota_worker.mjs
```

Hardware build and end-to-end ESP32-S3 test still required:
```bash
pio run -e Flowio-waveshare-esp32-s3
pio run -e Flowio-waveshare-esp32-s3 --target buildfs
```

Both binaries are necessary, because the new browser protocol is implemented
in `data/webinterface/app.js` (SPIFFS) **and** in the firmware. Use the
newly generated paired release ZIP, not an older package.

Field test matrix: ZIP success; induced upload disconnect; repeated ZIPs;
refusal on deliberate SHA/size mismatch; remote OTA after a failed ZIP
without restarting; remote OTA success; Nextion success. Capture a full
serial log and browser Console if a failure is reproduced.

**This patch is not hardware-validated until a complete PlatformIO build and
the actual ESP32-S3 tests pass.**

## ZIP update stage display (Web and local TFT)

The **remote HTTP paired OTA** and **Nextion** stage labels stay unchanged.
The local ZIP path has an independent 15-step journey in the Updates web
page, using the localized `updates.zipStage.*` keys in both `fr.json`
and `en.json`.

| Web step | ZIP backend stage | What really happens |
| --- | --- | --- |
| 1 | `zip_prepare` | Browser validates the ZIP structure |
| 2 | `zip_manifest` | Controller accepts the release manifest |
| 3 | `zip_spiffs_receive` | Receive SPIFFS in PSRAM, measured upload bytes |
| 4 | `zip_spiffs_ram_verify` | Verify complete SHA-256 before Flash |
| 5 | `zip_spiffs_prepare` | Wait for HTTP 202 acknowledgement; prepare inactive partition |
| 6 | `zip_spiffs_flash` | Write verified SPIFFS PSRAM buffer to Flash |
| 7 | `zip_spiffs_readback` | SHA-256 readback and candidate FS validation |
| 8 | `zip_firmware_receive` | Receive firmware into the recycled PSRAM buffer |
| 9 | `zip_firmware_ram_verify` | Verify complete firmware SHA-256 in PSRAM |
| 10 | `zip_firmware_prepare` | Prepare inactive firmware partition |
| 11 | `zip_firmware_flash` | Flash verified firmware with sequential erase |
| 12 | `zip_firmware_readback` | SHA-256 readback and firmware validation |
| 13 | `zip_activate` | Joint A/B activation after both images verify |
| 14 | `reboot` | ESP32-S3 restarts |
| 15 | `reconnect` | Web UI reconnects and confirms receipt |

`zip_spiffs_verified` and `zip_firmware_verified` are success aliases
for the corresponding readback stages. The TFT shows **14 stages**, without
the final *reconnect* stage (the device itself cannot show its own
reconnection before reboot). It retains the ZIP title through the reboot
stage even after the OTA status clears `ota_stage`.

During reception, writing and readback, progress shows a **real per-stage
percentage**; the combined Web progress does not move backwards between
images. The TFT keeps its black background, Flow.IO logo, bold bilingual
warning, 3D bar, and previous/current/next chronological layout without
redrawing the whole panel on each percentage change.

The HTTP 202 response means **staged and checked in PSRAM**, not already
written to Flash. The browser waits for the dedicated `verifying` and
`verified` states before sending the next image or the commit.

### Stage regression checks

```sh
node scripts/tests/check_zip_web_tft_stages.mjs
node scripts/tests/check_ota_ui_stages.mjs
node scripts/tests/check_tft_update_progress.mjs
node scripts/tests/check_zip_psram_preverify_async.mjs
node scripts/tests/simulate_zip_browser_poll.mjs
g++ -std=c++17 -Wall -Wextra -Werror -pedantic -Isrc test/host/tft_update_stages.cpp -o /tmp/tft_update_stages
/tmp/tft_update_stages
```

Source/static checks and simulated browser logic do not prove physical TFT
rendering or successful OTA on an ESP32. Full PlatformIO compile and paired
firmware+SPIFFS deployment remain necessary.

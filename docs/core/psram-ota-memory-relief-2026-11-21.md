# Flow.io — OTA PSRAM memory relief (2026-11-21)

## Scope
Branch: `feature/psram-internal-heap-relief-20261121`, fork `stephdust/Flow.io-Waveshare`.
Based on `feature/ota-two-step-legacy-migration-20261120`, including the OTA-NET source fix.

This is an **opt-in allocation change** for OTA JSON pools, not a global
`malloc` redirection, a FreeRTOS stack migration or a change to lwIP/AsyncTCP.

## Inventory before this change
Already explicitly in PSRAM:
- Firmware updater's persistent manifest metadata and response buffer.
- Remote paired-release manifest transfer buffer.
- ConfigStore metadata and its applyJson JSON document.
- Web server's route objects and several web/log data structures.

Updated on this branch:
- `runWaveshareUpdate_`: 2,048-byte release manifest JSON pool.
- `beginLocalRelease_`: second 2,048-byte release manifest JSON pool, which
  can coexist with the first during remote A/B OTA preparation.
- `runManifestCheck_`: variable-size manifest-check JSON pool.

All three now use the **existing** `Core/SpiRamJsonDocument.h` allocator,
with `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. If the pool allocation fails,
the update/check fails clearly rather than falling back into internal DRAM.
The remote paired flow keeps both JSON documents alive as before.

## What this can save (not a measured gain)
The paired manifest path has **4 KiB of JSON pool capacity concurrently**
(plus allocation overhead) whose location is now guaranteed to be PSRAM.
The manifest-check pool is variable size. The previous default
`DynamicJsonDocument` allocator could have used internal DRAM *or* PSRAM,
depending on Arduino core heap thresholds. **No actual bytes saved have
been measured on hardware.** This is a placement guarantee.

## Keep internal
- OTA and Modbus TCP task stacks; Wi-Fi/lwIP/AsyncTCP internals.
- Flash/DMA-critical buffers and latency-sensitive scheduling structures.
- The existing per-chunk OTA write buffer (1 KiB on the worker stack).

Never enable PSRAM globally for all `malloc` calls or move FreeRTOS
stacks solely to maximize the PSRAM utilization percentage.

## Validation
Static source regression:
```bash
node scripts/tests/check_psram_ota_placement.mjs
```
A host assertion checks the three explicit PSRAM allocations, their
error branches, and unchanged critical task settings. This is not
a firmware compile or stress test.

For the A/B transaction state model:
```bash
node scripts/tests/simulate_ota_ab_contention.mjs
```
Its success verifies transaction invariants under synthetic faults,
**not** the ESP32-S3 memory allocator, RF/network timing, or flash cache.

To verify on the device, build with PlatformIO and compare **two logs with
the same firmware profile, release images and TCP conditions**, one from
the parent branch and one from this branch. Capture:
- `[OTA-MEM]` (internal free, largest block, min internal, PSRAM, stack).
- `[OTA-NET]` (enabled listener mask, actual clients, requests/replies,
  queue pressure and tick peak).
- `[OTA-TRACE]` (operation ID and transfer/flash stages).
- `[OTA-PSRAM]` (manifest-check pool capacity).

First compare two idle listeners, then two ports with active clients.
A port listening is not equivalent to two clients transferring Modbus
requests. Keep the existing working A/B pair and rescue path.

**Status:** source-level change only; requires a PlatformIO compile and
ESP32-S3 hardware validation before merging to stable.

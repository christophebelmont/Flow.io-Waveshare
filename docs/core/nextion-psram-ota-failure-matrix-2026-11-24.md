# Nextion OTA hardening — PSRAM, Modbus priority and failure-injection report

**Date:** 2026-11-24  
**Branch:** `stephdust/Flow.io-Waveshare:fix/nextion-psram-uart-priority-20261124`  
**Status (updated November 24, 2026):** the user reports a **successful Nextion firmware update on their hardware** after compiling this branch. Regression simulations and source checks also passed. Christophe's independent review, multi-model testing, simultaneous-OTA exclusions and hardware failure-injection checks remain pending.

## Incident reproduced by captured status

Two Nextion update attempts ended with `last_failure.stage=flashing` and `reason=get baudrate error`. The latter was followed by `reset_reason=7` (watchdog reset). Source review showed `ESPNexUpload` selecting `Serial1` while the normal HMI driver uses `Serial2` on Waveshare; Serial1 serves Modbus RS485. The older frontend also displayed an artificial **5%** in the flashing stage even with no bytes sent.

The UART ownership issue is consistent with the recorded failures and the subsequently successful Nextion update on the patched branch. The isolated root cause cannot be proven without a controlled comparison with the former firmware.

## Corrective design

1. Nextion and the paired ESP32 firmware+SPIFFS OTA share **one exclusive admission lock**. It covers pending jobs, active workers, manifest checks and local release transactions. Late aborts of failed old transactions cannot alter a newer update.
2. Any active updater disables new Modbus TCP sessions; the existing TCP server closes listeners/clients after its short grace period. The **RS485 scheduler also stops accepting new work**, while finishing an already active frame safely.
3. The full Nextion `.tft` is staged **in PSRAM** with at least **2 MiB spare** and a sufficiently large contiguous block. If insufficient, stop **before touching the Nextion**, with no risky streaming fallback.
4. The PSRAM download accepts at most **three total HTTP attempts**. Resume with `Range` only when a `206` response has the exact `Content-Range`; `200` after partial download restarts the buffer cleanly.
5. Validate the byte count and **mandatory manifest SHA-256** before any UART transfer. The project release generator already emits SHA-256 for each Nextion artifact.
6. Suspend normal HMI access, initialize `Serial2`, auto-detect the baud rate cooperatively (yield during waits), and transfer the staged image from memory.
7. Require the Nextion `0x05` acknowledgment for **every 4 KiB packet, including the final partial/exact packet**. Failure does not send a soft reset to a potentially incomplete image.
8. The web UI reports actual **phase-local byte percentages** for PSRAM download and UART transfer, plus verification, UART initialization and final acknowledgment. Overall percentage is a monotonic phase-weighted indicator, not an estimate of elapsed time; no fabricated 5% minimum at flashing start.
9. On any failure, release staging memory and exclusive OTA admission, return the HMI to its normal driver where possible, and reopen Modbus services.

## Simulation evidence

**4,495 deterministic model scenarios were executed** in two consecutive batches, without assertion failures.

| Category | Cases | Coverage |
|---|---:|---|
| PSRAM | 440 | Empty, exact reserve boundary, fragmented free block, real-sized TFTs, large artifacts |
| HTTP response policy | 1,350 | 200/206, wrong length, malformed/mismatched Content-Range, transient and permanent errors |
| HTTP sessions | 1,890 | Full download, resume after drop, 200 restart, three-attempt limit, random transport faults |
| UART | 112 | Baud failure, 4 KiB boundaries, partial/exact final blocks, write failures, missing final ACK |
| OTA exclusive admission | 660 | Busy flags, remote worker nesting, races, terminal error retry, stale abort protection |
| Progress | 21 | Download, verify, initial UART, sent bytes, ACK and restart |
| Modbus | 15 | Busy/idle transitions, 300 ms TCP quiescence, RS485 queued rejection and in-flight drain |
| Failure preconditions | 7 | No UART without complete staged/verified file; no success/reset without ACK |
| **Total** | **4,495** | Model-level checks passed |

Model tests use the intended state-transition rules. They cannot faithfully simulate electrical UART disturbances, actual radio contention, FreeRTOS scheduling, PSRAM fragmentation over uptime, real Nextion bootloader behavior, platform-specific compile errors, or hardware watchdogs.

## Committed regression scripts

- `node scripts/tests/simulate_nextion_ota_failure_matrix.mjs` — deterministic fault-injection model
- `node scripts/tests/check_nextion_ota_priority.mjs` — source-level guards for exclusive admission, HMI UART2, RS485 quiescence, SHA, ACK and progress
- Retain the existing OTA regressions: `check_modbus_ota_priority.mjs`, `check_ota_http_retry.mjs`, `check_ota_psram_preverify.mjs`.

## Hardware acceptance checklist (must pass before merging)

1. Build Waveshare ESP32-S3 with its production PlatformIO flags. Confirm flash and PSRAM budgets and firmware image sizes.
2. Baseline: ESP32 firmware+SPIFFS OTA still succeeds; Modbus TCP ports and RS485 polling recover after update/forced failure.
3. Nextion 3.5-, 5- and 7-inch supported TFT models: correct identity, accurate progress, full success, reboot, refreshed version.
4. During Nextion download and UART transfer, attempt remote paired OTA, SPIFFS migration, local ZIP upgrade, manifest refresh and Nextion restart. Each conflicting operation must be rejected while busy.
5. Inverse test: while a paired firmware+SPIFFS OTA is active, attempt Nextion update; it must be rejected without changing the panel.
6. Force Wi-Fi loss, HTTP 503, partial download and insufficient PSRAM. Verify safe pre-flash errors and complete Modbus recovery.
7. Force wrong SHA-256, UART no-response, missing block ACK and missing final ACK. Verify the correct failure reason and no false success.
8. Run Nextion OTA with two Modbus TCP ports open and heavy RS485 traffic; verify zero new RS485 submissions while busy and no watchdog reset.
9. Check runtime logs and `/api/fwupdate/status` after normal completion, error and reboot, including the persisted `last_failure` and `last_operation`.

**The user has now confirmed one successful live Nextion update.** This does not by itself validate every Nextion display model, watchdog stress condition or the concurrent firmware+SPIFFS OTA exclusion. Christophe's review and additional regression tests are recommended before merging.

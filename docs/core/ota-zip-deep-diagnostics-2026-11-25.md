# OTA / local ZIP diagnostic markers — 2026-11-25

This patch is **instrumentation only**. It preserves the paired firmware +
SPIFFS A/B state machine, SHA-256 checks, Nextion update path, scheduler and
Modbus configuration. It is intended to make one failed ZIP + subsequent OTA
attempt sufficient to isolate the failing stage.

## Log prefixes and correlation

Use the same **transaction ID** (txn), **operation ID** (op), target and
timestamp to correlate the stages. A ZIP browser upload is an HTTP stream, not
the remote update's single PSRAM pre-download.

| Prefix | Location | Diagnostic payload |
| --- | --- | --- |
| `[OTA-ZIP-BROWSER]` | Browser console | ZIP manifest, request route, bytes uploaded, HTTP status, elapsed time, abort response |
| `[OTA-ZIP-HTTP]` | AsyncTCP callbacks | Manifest accepted, image open, begin, offset and total, 256-KiB checkpoints, slow chunks, verified/failed HTTP response, commit and abort |
| `[OTA-ZIP-RPC]` | HTTP → fwupdate handoff | Call kind, txn, target, offset, request/reply timing, queue rejection bitmask, wait timeout, memory |
| `[OTA-ZIP-WORK]` | fwupdate task | Action start/end, elapsed time, stack high-water mark, RAM, PSRAM, failure reason, completion release of RPC gate |
| `[OTA-ZIP-PHASE]` | Paired release engine | Transaction prepared, erase/OTA begin, image ready, SHA/filesystem verification begin/end, boot commit |
| `[OTA-ZIP-CLEAN]` | Failed ZIP cleanup | Abort entry, SHA and OTA handle state, reset start/end, free memory |
| `[OTA-ZIP-FAIL]` | Error transition | Last valid stage, written byte count, Flash handle, failure reason |
| `[OTA-ZIP-IDLE]` | fwupdate watchdog | No upload progress for 10 seconds, repeated at most once per 15 seconds |
| `[OTA-ADMIT]` | Next OTA request | Accepted/denied, state, pending job, active ZIP transaction and worker flag |
| `[OTA-TRACE]`, `[OTA-RAM]`, `[OTA-HTTP]`, `[OTA-HEAP]`, `[OTA-MEM]` | Remote paired OTA | Download, PSRAM verification, writing, commitment, memory/stack already instrumented |

The six numerical RPC action kinds: 0 = BeginRelease, 1 = BeginImage,
2 = WriteImage, 3 = EndImage, 4 = Commit, 5 = Abort.

The RPC rejection mask uses:
- `0x01`: updater busy, `0x02`: remote job queued
- `0x04`: manifest check queued, `0x08`: Nextion reboot queued
- `0x10`: update start pending, `0x20`: another local RPC in flight
- `0x40`: manifest check active

These are diagnostic bits; they do not change admission decisions.

## How to capture a single reproducible failure

1. Compile and install the diagnostic build once. Keep the existing serial and
   Web log collection enabled; remember WebSocket logs can be coalesced/dropped
   under load. Do not rely on browser progress reaching 100% as proof of an
   accepted HTTP response.
2. Send the same local release ZIP. Do **not** restart after the failure.
3. Try remote paired firmware + SPIFFS OTA once, without refreshing the ESP.
4. Save all Flow.io logs including any restart/boot section; if the ZIP was
   rejected, capture the Web UI error message. In the browser developer console
   (F12 → Console), filter `OTA-ZIP-BROWSER` and save those entries as well.
5. Diagnose the last observed transition:
   - `image_progress` but no `image_verified`: HTTP upload, worker stall, or SHA check.
   - `verification_failed`: SPIFFS mount/manifest or SHA issue.
   - `abort_begin` but no `reset_end`: cleanup or Flash handle release.
   - `reset_end` and `completion` but later `[OTA-ADMIT] rejected`:
     read the individual admission bits and status.
   - `[OTA-ADMIT] accepted` but no subsequent `[OTA-TRACE]`:
     worker scheduling/admission handoff.
   - `[OTA-TRACE]` advances until reset: inspect boot reason, heap and stack.

## Logging cost controls

- Successful HTTP and Flash upload callbacks are summarized **every 256 KiB**,
  at the first and final blocks. There is **no log for every ~1.4-KiB chunk**.
- An individual RPC is logged at phase boundaries, when rejected/failed,
  or when slower than 300 ms **at most once every five seconds**. Worker
  delays above 250 ms and slow HTTP callbacks follow the same rate limit;
  a per-image count is retained in the final summary.
- The idle watchdog starts at 10 seconds without data and is rate-limited
  to one entry every 15 seconds. This avoids compounding AsyncTCP load.
- No additional Flash/NVS writes for diagnostics, no raw firmware bytes, no
  SHA secrets, no credentials or HTTP session tokens in logs.
- This instrumentation is temporary for investigation and should be removed
  or disabled after the underlying failure is fixed.

## Tests

```bash
node scripts/tests/check_zip_ab_release.mjs
node scripts/tests/simulate_local_zip_ota_worker.mjs
node scripts/tests/check_ota_deep_diagnostics.mjs
pio run -e Flowio-waveshare-esp32-s3
```

The Node tests cover source contracts and scheduling simulations, not an
ESP32 hardware flash or actual TCP transfer. A successful PlatformIO build and
one full ZIP + OTA recovery sequence are still required.

# OTA A/B + two Modbus TCP ports: targeted diagnostics (2026-11-21)

## Scope and evidence

Branch: `feature/ota-two-step-legacy-migration-20261120` of `stephdust/Flow.io-Waveshare`.
Maintainer reports **5 successful grouped OTA updates followed by an interruption
on the sixth**, more frequent when two Modbus TCP ports are listening.

Last status received after the interruption:
- `last_operation.result=interrupted` (the persistent receipt was still `running` at reboot);
- `app_slot=app1`, `boot_slot=app1`, `spiffs_valid=true`;
- `reset_reason=1` = `ESP_RST_POWERON`.

**This alone cannot distinguish an external power/reset event from a fault that
caused power loss.** No crash trace has been captured yet, and RAM/CPU pressure
is an unverified hypothesis. The old A/B pair remained usable.

Source review: firmware updater is pinned to core 0 with an 8 KiB task stack.
The Modbus TCP server runs on the default module core 1 with a 4 KiB stack.
It supports 3 listeners (502/503/504), maximum 2 TCP clients per port.
Socket I/O uses bounded nonblocking recv/send, and RS485 transactions pass
through the common Modbus scheduler. HTTP stream inactivity threshold is 15s.
An **enabled listener** and an **active client sending requests** are different
load conditions.

## New low-rate diagnostic markers

All extra markers are emitted **only during OTA** (the Modbus network sample is
one per ~5 seconds when OTA is busy). No new flash writes, NVS checkpoints, or
per-request Modbus traces are introduced by this instrumentation.

| Marker | Source | What to examine |
| --- | --- | --- |
| `[OTA-TRACE]` | OTA task | `op` operation ID, manifest, partition preparation start/end, HTTP headers, bytes received, maximum flash-write microseconds, slow flash-write count, SHA/image verify, commit, abort |
| `[OTA-MEM]` | OTA task | `phase`, core, internal free/largest/min, fragmentation estimate, PSRAM, remaining stack bytes |
| `[OTA-NET]` | Modbus task | enabled mask (502/503/504), clients per port, cumulative requests/replies, RS485 pending/rejected, peak Modbus tick time, internal heap, PSRAM and remaining stack |

The `[OTA-NET] enabled=110` mask means ports **502 and 503** are configured;
`clients=2,2,0` means **four active connections**. Port numbers follow
index order 502/503/504; the bitmask is **not** a count of TCP connections.
`tick_peak_us` and `slow_ticks` are **task latency proxies, not whole-core
CPU utilization percentages**. `frag_pct` is only an estimate from heap
size/largest block.

Interpretation:
1. Last `stage=partition_prepare_start` without `...end`: inspect flash erase /
   `esp_ota_begin` blocking; compare Modbus tick peak.
2. `stage=stream_progress` followed by `HTTP waiting`, `HTTP interrupted`
   or abort: compare clients/RS485 backlog and internal largest free block.
3. `flash_max_us` rising sharply: flash write latency, possibly worsened
   by concurrent networking; does not itself establish causation.
4. `stage=image_verify_end success=1` for both images and then
   `stage=commit_start`: inspect NVS receipt persistence / boot-partition
   selection if reboot does not follow.
5. Serial log stops abruptly with no clean `abort` and next boot reports
   `reset_reason=1`: investigate power/reset path as well as firmware;
   serial captures help locate the last active operation.

## Host simulations (not device validation)

Run `node scripts/tests/simulate_ota_ab_contention.mjs`.
Deterministic virtual-time OTA state model using actual release image sizes
(1,572,864-byte SPIFFS and 2,816,944-byte firmware) and 1024-byte chunks.
For each of 0, 1 and 2 Modbus listening ports it tests 100 seeds for each of
10 scenarios: normal, 14.999s pause, 15.001s pause, truncated SPIFFS at
271,215 bytes, corrupt hash, allocation refusal, reset during download,
reset between images, reset before commit and reset after commit.

**Results of the model:** 3,000 scenario runs, 900 successful paired commits,
1,200 deliberate failures and 900 deliberate interruptions. All transaction
safety assertions pass: failed/incomplete images do not switch the boot
partition. Injected faults are counted as expected outcomes, **not observed
firmware error rates**. The model does *not* run ESP-IDF, lwIP, the Arduino
HTTPClient, FreeRTOS scheduler, actual flash, memory allocator, Wi-Fi, or
live Modbus TCP traffic; it cannot confirm the cause of the real reset.

## Next useful hardware test — one capture, two ports

1. Build the diagnostic branch. For native USB console only, consider
   `-D ARDUINO_USB_CDC_ON_BOOT=1` with the existing
   `-D ARDUINO_USB_MODE=1`; confirm which COM port now carries logs.
2. Start the USB serial capture **before** pressing the grouped OTA button;
   capture also the next boot. Keep the Modbus 502 and 503 listeners enabled,
   and record whether their TCP clients actually remain connected.
3. After any interruption or success, save `/fwupdate/status`. Save the
   USB capture (e.g. `ota_serial.log`). Optionally run
   `node scripts/tests/summarize_ota_trace.mjs ota_serial.log`
   for a quick summary; keep the full raw log.
4. Match the `[OTA-TRACE]` operation ID, the last `[OTA-MEM]` snapshot and
   the nearest `[OTA-NET]` line. Do not infer RAM exhaustion solely from
   `reset_reason=1`.
5. If no anomaly appears with two *idle* listeners, compare with two ports
   actually receiving requests. Avoid making this a continuous stress test
   without the ability to recover a firmware that stops answering.

**Status:** diagnostic source commits only; not compiled in the assistant's
environment and not hardware-validated. Keep the previous stable A/B image
and rescue path. Do not promote this diagnostic branch to stable yet.

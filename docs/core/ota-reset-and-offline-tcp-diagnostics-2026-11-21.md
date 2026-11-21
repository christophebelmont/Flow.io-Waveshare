# Flow.io OTA — reboot-origin and inactive-TCP diagnostics (2026-11-21)

## Context

The lab serial capture `device-monitor-261121-181559.log` contains eight OTA A/B transactions.
Seven complete; transaction **367635983** resets unexpectedly during firmware
streaming (about 84%). Pre-reset OTA heap and flash write latency do not
demonstrate an OOM. Prior diagnostics did not emit `[OTA-NET]` when the
Modbus TCP task was running but had no listener: the logging code was
located after early returns for absent station IP, disabled ports, or
an ineligible Modbus master.

This change adds **observability only**. No OTA flash protocol, priority,
network socket, reboot scheduling, RAM allocator, or task stack change.

## Next-boot retained breadcrumb: [BOOT-RESET]

Example of an *unexpected* software reset mid-download:

```text
[BOOT-RESET] reset_reason=3 rtc_valid=1 planned=none planned_op=0 last_ota_op=367635983 stage=firmware_write received=2363075/2826272
```

Example of a planned A/B reboot:

```text
[BOOT-RESET] reset_reason=3 rtc_valid=1 planned=ota_release planned_op=367635984 last_ota_op=367635984 stage=commit received=0/0
```

Fields:
- `reset_reason` — ESP-IDF `esp_reset_reason()` numeric code.
- `rtc_valid` — RTC no-init record passed magic, version and checksum.
- `planned` — `none`, `ota_release`, `ota_nextion`, `system`, or `factory_reset`.
- `planned_op` — firmware updater operation ID for a planned OTA restart.
- `last_ota_op`, `stage`, `received/total` — last checkpoint from the preceding boot.

The marker is **one-shot**: after reporting it, firmware invalidates it.
It is recorded at manifest start, partition preparation, each 256 KiB
of firmware or SPIFFS transfer, verification, and commit. It may be
slightly behind the true byte offset at a reset.

Stored in `RTC_NOINIT_ATTR` RAM, **not** in NVS or flash. No extra flash
wear and no blocking storage transaction while OTA is writing. Software
resets ordinarily retain this memory; a power cycle / power loss can
erase it, so `rtc_valid=0` does **not** itself prove anything about
the reset origin. Similarly, `planned=none` only means the instrumented
OTA/System paths did not request an intentional reboot.

## OTA-time TCP: [OTA-NET]

The modbustcp task now samples `[OTA-NET]` every **5 seconds** while
the firmware update service reports busy, even when there is no active
port or no station IP. The existing `rxreq`, `txreply`, `clients`,
RS485 queue, heap, PSRAM, and stack fields remain available.

New / clarified fields:
- `enabled=xyz` — configuration for ports **502, 503, 504** in order,
  where 1 means configured enabled; e.g., `110`.
- `station=0|1` — the network service is in station mode.
- `eligible=0|1` — underlying Modbus master service is available.
- `listening=0|1` — the module has started its socket server while
  a station connection exists; it is **not a per-port bind success test**.
- `clients=a,b,c` — established clients on 502, 503, 504 respectively.

Examples:
- `enabled=000 station=1 listening=0 clients=0,0,0`: no ports configured.
- `enabled=110 station=1 listening=1 clients=0,0,0`: two listener ports
  configured, but no client requests observed.
- `enabled=110 station=1 listening=1 clients=1,1,0`: one connected
  client per configured port, for an active-contention comparison.

The diagnostic runs from the existing Modbus TCP task; when that task
itself is absent or never scheduled, it cannot emit `[OTA-NET]`. Its
absence therefore still matters and should be paired with the normal
`startTask ... task=modbustcp` startup log.

## Validation

Host source-level regression:

```bash
node scripts/tests/check_ota_reboot_diags.mjs
```

A full PlatformIO build and ESP32-S3 test are required before merging.
For the lab test, leave `pio device monitor -f log2file` open through
the reboot, then compare `[BOOT-RESET]`, `[OTA-TRACE]`,
`[OTA-MEM]`, and `[OTA-NET]` by operation ID.

No physical RS485 devices are connected on the lab bench; their timeout
errors are expected. They may still contribute scheduling load, but
must not be treated as evidence of a Modbus line fault.

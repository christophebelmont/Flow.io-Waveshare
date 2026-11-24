# Family 5 — Home Assistant discovery refresh and PSRAM validation

## Scope

Family 5 extends one-shot Home Assistant MQTT Discovery while preserving RAM headroom on the ESP32-S3. The ConfigStore HA page includes a **Refresh HA entities** action (not a persistent configuration value).

- `POST /api/ha/discovery/refresh` returns `202` with `{"ok":true,"queued":true}` if the discovery catalogue is still active.
- When the one-shot catalogue has been released, the endpoint returns HTTP `409` with `restart_required:true`. The browser explicitly asks for confirmation before invoking `POST /api/system/reboot`.
- The browser does not silently retry a refresh POST; this prevents accidental duplicate requests.
- HA entity tables, deferred retry bitsets, transient MQTT-config JSON and discovery JSON pools use PSRAM. Releasing one-shot resources is intentional; they must not be reconstructed from a partial catalogue after startup.
- Deferred discovery retries are bounded (four passes with a five-second interval), prioritizing ready entities.

## Source checks

Run from the repository root:

```sh
python3 -m unittest scripts/tests/test_ha_discovery_retries.py
python3 -m unittest scripts/tests/test_ha_background.py scripts/tests/test_ha_dynamic_storage.py scripts/tests/test_ha_discovery_topics.py
```

These are source/host guards, **not** a firmware-build or hardware validation. Build the Waveshare ESP32-S3 target with PlatformIO and validate on the device before merging.

## Hardware sanity check

1. Boot Flow.io with MQTT and Home Assistant enabled and the broker reachable. Record internal-heap free/largest allocation and PSRAM free before, during and after HA discovery.
2. Verify all expected HA entities and retained discovery topics, including entities whose underlying sensors are temporarily unavailable.
3. Inspect the logs for bounded deferred retries and the one-shot `HA PSRAM released` accounting log. Check no reboot loop, queue saturation, broker disconnect or watchdog reset.
4. Open **ConfigStore → Home Assistant** and press **Refresh HA entities**. After one-shot release, confirm the UI requests approval to reboot and clearly warns about unsaved changes.
5. Cancel confirmation: verify no reboot. Confirm it: verify the controlled reboot and that all HA entities are republished automatically after reconnecting to MQTT.
6. Check the action is disabled when Home Assistant is disabled. Verify FR and EN texts.
7. Repeat while the broker is disconnected, then reconnect. Verify bounded retry behaviour and eventual discovery without losing MQTT connectivity.

## Acceptance criteria

- No duplicate discovery floods, heap exhaustion, stack overflow or degraded Modbus/OTA/network responsiveness.
- The HA refresh action does not alter saved ConfigStore values.
- All expected HA entities are published after the one-shot rebuild.
- Free PSRAM increases after the one-shot table and retry bitmaps are released.
- Firmware compilation, actual hardware behaviour and HA/MQTT broker results are recorded separately from source-code guard results.

## Status

**Branch for lab testing:** `stephdust/Flow.io-Waveshare:fix/family5-ha-discovery-psram-20261124`, created directly from `christophebelmont/Flow.io-Waveshare:dev` at `b6df88da` (2026-11-24). This isolates the eight Family 5 files and includes Christophe's latest unrelated `app-core.js` change without merging other pending PRs.

**Source review:** 19 targeted static consistency checks passed, covering production one-shot mode, PSRAM allocations and release, four deferred retries at five-second intervals, MQTT drop callbacks, registered unavailable-value fallback, refresh action and reboot confirmation. These checks inspect source contracts; they **do not execute the ESP32 firmware or constitute the Python test suite**.

**Still required:** run the Python test commands above, compile with PlatformIO, and verify on an ESP32-S3 with real MQTT/Home Assistant including broker disconnections, missing initial sensor measurements, discovery refresh and PSRAM recovery. Do not mark Family 5 as production-ready until those checks are recorded.

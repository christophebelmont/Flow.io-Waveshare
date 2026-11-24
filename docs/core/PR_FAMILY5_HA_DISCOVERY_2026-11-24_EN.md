# Pull Request — Family 5: resilient HA discovery and ConfigStore refresh

**Suggested title:** `fix(ha): resilient one-shot discovery, PSRAM cleanup and refresh action`  
**Base:** `christophebelmont/Flow.io-Waveshare:dev`  
**Head:** `stephdust:fix/family5-ha-discovery-psram-20261124`

## Summary

Improve Home Assistant MQTT Discovery reliability and reclaim temporary ESP32-S3 PSRAM after one-shot publication. Add a safe, localized **Refresh HA Entities** action to the ConfigStore interface.

## Changes

- Up to **four deferred discovery passes**, five seconds apart, without blocking ready entities.
- Handle temporarily missing sensor values, empty catalogues, and incomplete publication after retry exhaustion.
- Allocate HA entity tables, JSON buffers and pending/deferred bitmaps in PSRAM, and release one-shot resources afterward.
- ConfigStore refresh button (FR/EN), with a **single POST** and explicit reboot confirmation once the catalogue has been released.
- Responsive UI fix: refresh status appears on a full-width line below the button.

## Validation

- **User confirms the latest Family 5 build and on-device behavior are OK.**
- **2,384 deterministic simulation scenarios**, **369,957 model assertions**, and **25 source-level consistency checks** passed.
- Additional review of MQTT disconnections, missing sensor readings, PSRAM usage, and repeated discovery on real hardware is recommended.

## Documentation

- [Short English changelog](CHANGELOG_FAMILY5_HA_DISCOVERY_2026-11-24_EN.md)
- [French simulation report](family5-ha-discovery-simulations-2026-11-24.md)
- [Validation checklist](family5-ha-discovery-validation.md)

A separate **three-page French explanatory PDF** has been prepared for Christophe and can be attached to the PR discussion.

## Scope

No changes to Modbus protocol, TFT or Nextion features, OTA implementation, MQTT discovery topic IDs or persisted configuration schema. Independent of the other pending PRs.

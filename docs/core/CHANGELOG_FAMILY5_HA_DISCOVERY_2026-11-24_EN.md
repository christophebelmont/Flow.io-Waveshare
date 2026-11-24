# Changelog — Family 5: Home Assistant Discovery

**Date:** November 24, 2026

## Improvements

- Retry MQTT Discovery for temporarily unavailable entities: **up to four passes**, five seconds apart, without blocking ready entities.
- Allocate transient HA entity tables, JSON buffers and retry bitmaps in **PSRAM**; release them after one-shot discovery.
- Handle empty catalogues and report **incomplete discovery** with the number of entities still missing after retry exhaustion.
- Add a **Refresh HA Entities** button in ConfigStore (FR/EN), with explicit reboot confirmation when the one-shot catalogue has been released.
- Fix the refresh action's responsive layout so status messages appear on a full-width line beneath the button.
- Keep refresh requests single-shot; no automatic duplicate POST or persisted configuration change.

## Validation

- User confirms the complete Family 5 build and on-device behaviour are OK.
- **2,384 deterministic simulation scenarios**, **369,957 model assertions** and **25 source-level checks** passed during development.
- Independent review and broader broker/disconnection regression testing remain recommended.

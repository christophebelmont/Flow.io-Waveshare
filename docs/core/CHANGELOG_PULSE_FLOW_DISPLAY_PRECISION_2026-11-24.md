# Changelog — Pulse counter and flow display precision

**Date:** November 24, 2026  
**Target:** `christophebelmont/Flow.io-Waveshare:dev`  
**Source:** `stephdust/Flow.io-Waveshare:fix/digital-pulse-flow-display-precision-20261124`

## Summary

Remove excessive decimal digits from pulse frequency and converted-flow values displayed or published by Flow.io. The change uses the existing ValueRegistry metadata precision and shared serialization pipeline; **raw measurements and calculations remain unchanged**.

## Changes

- **Pulse frequency (`iXX.rate`):** use a fixed presentation precision of **2 decimal places**, in both Mode 1 (cumulative pulse counter) and Mode 2 (instantaneous flow).
- **Converted flow (`iXX.flow`):** follow the digital input's configured `Précision` value, clamped to **0–6 decimals**, in both modes.
- Apply precision metadata at value registration and refresh converted-flow presentation precision when its setting changes at runtime; avoid repeatedly updating unchanged metadata.
- Let the existing shared numeric formatting path supply the same presentation to the Web Dashboard, local data value views, TFT and MQTT/Home Assistant, without rounding stored ValueRegistry values.

### Example

For a raw pulse frequency of `2.716776132583618` pulses/minute:

| Value | Previously | Now |
|---|---|---|
| `i01.rate` | `2.716776132583618 imp/min` | `2.72 imp/min` |
| `i01.flow` with configured precision 2 | `2.716776132583618` | `2.72` |
| `i01.flow` with configured precision 0 | `2.716776132583618` | `3` |

The `iXX.flow` examples assume a converted result numerically equal to the sample frequency, such as a factor of 1. Units are unchanged.

## Files changed

- `src/Modules/IOModule/IOModule.cpp`
- `src/Modules/IOModule/IOModule.h`
- `scripts/tests/test_pulse_display_precision.py` — source-level guards
- `test/host/value_pipeline.cpp` — numerical presentation and raw-value preservation regression checks

## Testing and review

- **13 source-level consistency checks** passed at patch preparation.
- A C++ host regression case was added to assert `2.72` in serialized JSON while raw ValueRegistry values remain unrounded, plus a precision-change check; running that new case in CI remains to be confirmed.
- The user reports the fix **appears to work** in their environment.
- **Christophe's independent review and validation are pending**, including any PlatformIO compilation, MQTT/HA entity display checks and final on-device testing.

## Scope and compatibility

Presentation metadata only. No change to pulse counting, the flow conversion formula, GPIO, NVS counter persistence, MQTT topic IDs, discovery IDs or the stored measurement values. No configuration migration is required. The pending Family 5 work and other feature PRs are not included.

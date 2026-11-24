# Changelog — Digital input pulse counter and instantaneous flow documentation

**Date:** November 24, 2026  
**Target:** `christophebelmont/Flow.io-Waveshare:dev`  
**Source:** `stephdust/Flow.io-Waveshare:docs-input-counter-vs-flow-20261124`

## Summary

Clarify the difference between digital input **Mode 1 — Pulse Counter** and **Mode 2 — Instantaneous Flow** in French reference documentation and both French and English configuration help texts.

## Documentation updates

- Added a side-by-side explanation of the measured values, persistence, reset functionality, typical uses and conversion formulas in `docs/modules/IOModule.md`.
- Documented a common water meter example (10 pulses per liter) for both modes:
  - **Mode 1:** `C0 = 0.1` liters per pulse. A count of 250 pulses represents 25 liters.
  - **Mode 2:** `C0 = 10` pulses per liter. At 120 pulses/min, flow is 12 liters/min.
- Corrected `docs/core/flow-mode-2-instantaneous-rate-2026-11-23.md`: in Mode 2, the actual firmware uses **K / C0 = pulses per liter**, strictly positive, and **divides** the pulse frequency by K. The previous technical document incorrectly stated liters per pulse and multiplication.
- Clarified that Mode 1 maintains a cumulative pulse count with NVS checkpoints, while Mode 2 retains no exposed or persistent cumulative total and computes flow using approximately one-second windows.

## Configuration help corrections

- Corrected **16 C0 help descriptions** and **16 flow-unit help descriptions** in **each** of the FR and EN translations: `src/Modules/IOModule/text/i18n.fr.json` and `i18n.en.json`.
- Explained that `C0` is a **multiplier in Mode 1** (liters/pulse for a total in liters), but a **divisor in Mode 2** (pulses/liter). Warned against reusing the same value when switching modes.

## Scope and verification

**Documentation and user-interface help only.** There are no numerical calculation, timing, GPIO, persistence, protocol, or data model changes. No configuration migration is required.

- Both modified translation files were parsed and their 16 C0 keys plus 16 flow-unit keys were checked for presence.
- Formulas and terminology were cross-checked against `PulseFlowUnits.h`, `PulseRateOnly.h` and `IOModule.cpp`.
- No PlatformIO build or hardware regression run is claimed for this documentation update.

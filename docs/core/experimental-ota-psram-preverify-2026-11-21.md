# Experimental OTA: HTTP -> PSRAM -> SHA-256 -> inactive A/B flash

**Branch:** `experiment/ota-psram-preverify-20261121`  
**Parent:** `feature/psram-internal-heap-relief-20261121` (`574983d`)  
**Status:** implemented for laboratory evaluation; ESP32-S3 PlatformIO build and
hardware validation are **not** yet completed.

## Motivation and scope

The lab observed successful paired A/B OTA updates with a strong Wi-Fi signal,
but also occasional HTTP stalls, retransmissions, and unexpected restarts.
The prior updater wrote each 1024-byte HTTP chunk directly to the inactive
SPIFFS/application partition. The experimental path separates the network
transfer and flash-writing phases to reduce overlap.

**Only remote paired Waveshare OTA** (manifest + SPIFFS + firmware downloaded
by the ESP32) uses the new PSRAM path. Local browser ZIP installation and
Nextion OTA retain their existing implementations.

The parent branch's Wi-Fi STA recovery fix (`2ba147e`, originally
`c669611`) is retained unchanged. The captive portal must not replace STA
after a temporary loss of a previously functioning configured Wi-Fi network.

## Per-image stages

1. Download exactly `Content-Length` bytes to a contiguous PSRAM buffer.
2. Close the HTTP socket.
3. Recompute SHA-256 from the complete PSRAM image in 16 KiB segments.
4. Compare it to the SHA-256 in the validated paired release manifest.
5. **Only on match**, prepare/erase the inactive A/B partition.
6. Copy 1024 bytes PSRAM -> internal stack buffer and call the **existing**
   `writeLocalImage_` method, yielding between chunks.
7. Call the existing `endLocalImage_` SHA-256 and SPIFFS/app validation.
8. Release that image's PSRAM buffer before starting the next image.
9. Only after **both** images pass, call the existing
   `commitLocalRelease_` and schedule one reboot.

At most one complete image is buffered at a time. The firmware image is
the largest (approximately 2.83 MB for the current test release). Our lab
observed around 5.7 MB PSRAM free during the legacy OTA.

### Memory policy and fallback

`OtaPsramPolicy::canStageImage` enforces all conditions:
- image size nonzero;
- enough free PSRAM for image **plus 2 MiB reserved**;
- largest free contiguous PSRAM block can hold the entire image.

Allocation uses `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`
and is freed automatically on every path. If policy or allocation fails,
that **whole image** uses the unmodified 1024-byte streaming code. This is
logged as `stage=fallback_stream`. A download failure or SHA mismatch does
**not** trigger fallback; it causes the release to abort and leaves the
currently running A/B slot untouched.

The two OTA code paths share one 1 KiB internal stack chunk. The PSRAM
buffer is **never passed directly** to `esp_ota_write` or
`esp_partition_write`; each flash write reads the small internal copy.
No NVS staging writes are added.

### Logs

```text
[OTA-RAM] ... stage=download_begin mode=psram size=... free_before=... largest_before=... free_after=... reserve=2097152
[OTA-RAM] ... stage=download_progress received=.../...
[OTA-RAM] ... stage=download_end success=1 bytes=.../... ms=...
[OTA-RAM] ... stage=preverify_end sha256=OK ms=... flash_touched=0
[OTA-RAM] ... stage=flash_prepare_start
[OTA-RAM] ... stage=flash_progress flashed=.../... flash_max_us=... slow20ms=...
[OTA-RAM] ... stage=image_end mode=psram success=1 download_ms=... preverify_ms=... flash_prepare_ms=... flash_write_ms=... flash_max_us=... free_psram=...
```

If contiguous allocation fails:

```text
[OTA-RAM] ... stage=fallback_stream reason=psram_reserve_or_alloc ...
```

Also inspect `[OTA-MEM]`, `[OTA-NET]`, `[BOOT-RESET]` and `[UPGRADE]`.
RTC breadcrumbs identify the new RAM download/verification stages, although
previous testing showed `rtc_valid=0` after a reset on some boards.

The Web progress percentage is intentionally based on **bytes written to A/B**;
it does not advance during a PSRAM download, avoiding a misleading rollback
in the displayed percentage when flash writing begins.

## Validation checklist

Run the source-contract regression:

```bash
node scripts/tests/check_ota_psram_preverify.mjs
```

The test optionally compiles the PSRAM reserve policy with a host C++ compiler.
Then run a full PlatformIO compile (correct board profile, USB CDC setting)
and capture a real OTA with the USB serial monitor and ideally tcpdump.

Suggested scenarios:
- Strong Wi-Fi: verify both images use `mode=psram`, SHA preverify, A/B
  commit, restart and correct mounted SPIFFS pairing.
- Force memory pressure safely: confirm `fallback_stream` is logged and
  the legacy OTA path remains operational.
- Interrupt HTTP mid-buffer: the inactive partition for that image must
  **not** begin erase or write; release aborts without reboot.
- Corrupt an artifact against its manifest SHA: preverification must fail
  before flash for that image.
- Test A -> B and B -> A.
- Disable AP or temporarily interrupt Wi-Fi after an established connection:
  the pre-existing STA reconnection fix remains in effect.
- Compare `download_ms`, `preverify_ms`, `flash_write_ms`,
  `flash_max_us`, internal heap and PSRAM with the parent branch.
- Test ZIP A/B separately: it deliberately does **not** use this RAM staging.

No automatic merge or release should happen until these tests pass on hardware.

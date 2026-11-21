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

### HTTP retry, resume and restart

The remote HTTP manifest, SPIFFS and firmware each use **three attempts
total**, including the initial request. A transient connection failure or
HTTP 408/429/5xx schedules another attempt (500 ms delay) without changing
the active A/B partition. Permanent errors such as 403/404, invalid
Content-Length, or an inconsistent HTTP 206 response fail immediately.

For **binary images** with a partially completed transfer:

- Send `Range: bytes=<received>-`.
- Accept HTTP **206** only when `Content-Range`, `Content-Length`,
  original total size, requested offset and final byte all agree.
- If the server ignores Range and replies **200** with the complete image:
  - in PSRAM mode, start filling the same buffer again from byte zero;
  - in fallback streaming mode, re-download and discard the previously
    flashed prefix; the final manifest SHA-256 still covers the whole image.
- A broken connection is fully closed before the next attempt.
- A flash write error is not a transport error and is never retried.
- SHA mismatch on a completed image aborts the release; it does not trigger
  repeated writes or an automatic boot switch.
- The retry counter resets for each image. A reboot loses the RAM buffer;
  HTTP Range is **not** a persistent resume-across-reboot mechanism.

The tiny JSON manifest is always restarted from byte zero.

Trace examples:

```text
[OTA-HTTP] ... target=spiffs mode=psram attempt=2/3 offset=365991
[OTA-HTTP] ... target=spiffs attempt=2 code=206 ... mode=2
[OTA-HTTP] ... target=firmware attempt=2 stage=restart_full server_no_range=1
[OTA-HTTP] ... stage=retry_scheduled next=3/3 offset=...
[OTA-HTTP] ... stage=retries_exhausted attempts=3 ...
```

The common policy and HTTP 200/206 tests are in
`scripts/tests/check_ota_http_retry.mjs`.

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

## Protection de migration contre SPIFFS-first

**Cette protection concerne les anciens firmwares déjà déployés** : ils
téléchargent `flowios3-spiffs-<version>.bin` puis effacent directement leur
partition SPIFFS **active**. Modifier le firmware expérimental ne peut pas
corriger rétroactivement leur comportement.

Le build publie désormais **deux chemins vers la même image** :

- `binary/flowios3-spiffs-<version>.bin` — nom historique, encore affiché
  par les anciennes interfaces Web, mais **refusé par le serveur protégé** ;
- `binary/flowio-paired-spiffs-<version>.bin` — copie identique (même
  SHA-256) référencée par `flowio-release-<version>.json` et `release.json`,
  téléchargée uniquement par le nouvel OTA A/B vérifié.

Le package ZIP continue à contenir `spiffs.bin` comme avant.
`manifest.json` conserve les entrées historiques pour la migration :
le bouton SPIFFS de l'ancien Web sera donc encore visible, **mais son GET
reçoit HTTP 403** avant l'effacement. Le nouveau firmware, une fois démarré,
convertit ce bouton en lancement d'une release A/B complète via le manifeste
versionné. Les nouvelles images ne peuvent pas être réutilisées par le
mécanisme direct dangereux.

### Lancer le serveur protégé

Après avoir régénéré et copié **tous** les artefacts `binary/` du build :

```sh
# Arrêter d'abord le précédent "python -m http.server 90".
python scripts/serve_ota_guarded.py --root . --port 90 --bind 0.0.0.0
```

Le serveur doit être lancé depuis la racine qui contient `binary/`, ou
indiquer le bon dossier avec `--root`. L'accès HTTP depuis l'ESP32 reste
`http://ADRESSE_DU_PC:90/binary/...`. Garder ce service sur un LAN fiable :
ce serveur n'implémente pas d'authentification HTTP.

**Si l'ancien serveur `python -m http.server` reste actif, le garde-fou
n'est PAS actif.** Sur un autre serveur Web, la même règle de refus
HTTP 403 pour `*-spiffs-<version>.bin` doit être configurée en conservant
l'alias `flowio-paired-spiffs-<version>.bin` accessible.

### Procédure après activation du garde-fou

1. Depuis l'ancien firmware, cliquer sur **Firmware**, pas SPIFFS.
   Le mécanisme historique clone le SPIFFS en slot inactif.
2. Attendre le redémarrage du firmware intermédiaire, vérifier son statut et
   que `migration_pending=true` si l'API l'expose.
3. Cliquer sur **SPIFFS** dans l'ancienne interface clonée : le nouveau
   firmware intercepte désormais cette route et lance l'installation
   **complète firmware + SPIFFS A/B** à partir du manifeste. Le serveur
   autorise le nouvel alias binaire.
4. Vérifier `migration_pending=false`, `spiffs_valid=true` et le
   changement de slot de démarrage.

Un clic sur SPIFFS au **premier** stade renvoie HTTP 403 sans effacer le
filesystem. La protection ne s'applique pas à un ancien firmware pointant
vers un autre serveur, ni à une requête servie par un serveur non protégé.

Tester le serveur HTTP de façon réelle (GET/HEAD locaux) :

```sh
python -m unittest scripts.tests.test_export_binaries
python -m unittest scripts.tests.test_ota_guarded_server
```

## Validation checklist

Run the source-contract regression:

```bash
node scripts/tests/check_ota_psram_preverify.mjs
node scripts/tests/check_ota_http_retry.mjs
```

The test optionally compiles the PSRAM reserve policy with a host C++ compiler.
Then run a full PlatformIO compile (correct board profile, USB CDC setting)
and capture a real OTA with the USB serial monitor and ideally tcpdump.

Suggested scenarios:
- A failing HTTP server which ignores Range: verify at most 3 attempts,
  with a fresh HTTP 200 download or streaming prefix replay.
- A Range-capable HTTP server: interrupt the first request and verify a
  strict HTTP 206/Content-Range resumes at the correct byte.
- A mismatched 206 header, HTTP 404, or invalid image size: abort safely
  without retrying or committing A/B.
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

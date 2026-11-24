# Detailed changelog — Flow.io log capture toolkit

## 2026-11-24 — Initial repository contribution

**Scope:** Documentation and external diagnostic tools only. This change adds standalone log collectors for Linux and Windows; it does **not** change Flow.io firmware, ESP32-S3 build settings, RS485/Modbus behavior, Web APIs, SPIFFS assets, or OTA implementation.

### Files included

| File | Purpose |
| --- | --- |
| `collecte_logs_flowio.sh` | Linux Bash collector using curl, jq and flock. |
| `collecte_logs_flowio.ps1` | Windows PowerShell 5.1 / PowerShell 7+ collector without third-party modules. |
| `README.md` | French setup instructions, operation and file layout for both platforms. |
| `.gitignore` | Prevents locally generated `flowio-logs/` archives, state and lock files from entering Git. |
| `CHANGELOG.md` | This detailed record of capabilities, limitations and scope. |

### 1. Continuous runtime-log retrieval

- Authenticates with Flow.io via `POST /api/auth/login` using a Web session/cookies.
- Queries `GET /api/logs/runtime?after=<cursor>&limit=64` and retrieves additional pages, capped at eight pages per polling cycle to avoid excessive device/network work.
- Default polling interval: **5 seconds**, configurable at the top of either script.
- Appends collected records to `flowio-logs/runtime/flowio-logs.log`; does not replace that file on collector restart.
- Prepends an ISO UTC collection timestamp and the `boot_id` and sequence number to each log record. Original Flow.io timestamps and message text remain in the record.
- Removes ANSI color escape sequences and makes embedded line breaks safe for a line-oriented text log.
- Reports any detected runtime ring-buffer losses (`lost`/oldest sequence) when the Flow.io PSRAM history has been overwritten before retrieval.

### 2. Per-boot log archives

- Reads `GET /api/fwupdate/status` to detect the current `boot_id` and identify Flow.io restarts.
- Waits until `GET /api/logs/boot/status` reports that the boot capture is **complete**.
- Retrieves the boot capture in pages via `GET /api/logs/boot?offset=<offset>&limit=32`.
- Validates the page offsets and record counts; checks the current `boot_id` again after the transfer so records from separate restarts are not mixed.
- Writes to a temporary file and publishes the completed archive as `flowio-logs/boot/boot-<boot_id>.log`.
- Skips an archive already saved for that boot; retries later if the capture is incomplete or interrupted. Includes captured-entry and dropped-entry counts in the archive.

### 3. Resumption, duplicate prevention and failure handling

- Maintains the last observed boot ID and runtime cursor in `flowio-logs/control/collector.state`.
- At collector startup, reads the persisted state and examines the end of the runtime log to recover a cursor where log lines were appended but the state file had not yet been updated.
- Resets the cursor when a different Flow.io `boot_id` is detected.
- Uses a single-process lock (`flock` on Linux; exclusive file handle on Windows) to avoid duplicate simultaneous collectors targeting the same output directory.
- Retries HTTP requests after an authentication/session problem by logging in again. Network failures are reported and retried in later polling cycles.
- Uses bounded HTTP request timeouts (Linux: 4 s connection, 15 s overall; PowerShell: 15 s request timeout).
- Checks incoming API response structures before advancing the runtime cursor, reducing the risk of silently skipping records on malformed pages.

### 4. Files and directory isolation

Both scripts create local, OS-side output directories, separate from the Flow.io device:

```text
Capture_Flow.io_Logs/
  collecte_logs_flowio.sh
  collecte_logs_flowio.ps1
  README.md
  CHANGELOG.md
  .gitignore
  flowio-logs/                     # local output; ignored by Git
    runtime/flowio-logs.log        # appended runtime event stream
    boot/boot-<boot_id>.log        # completed boot snapshots
    control/collector.state        # saved progress
    control/collector.lock         # single-instance lock
```

When started with redirected output, the Linux/Windows launch commands can also create `control/collecte-flowio.out` and `control/collecte-flowio.err`.

### 5. Supported environments and configuration

**Linux:** Bash with `curl`, `jq`, `flock` (util-linux), `sed` and standard core utilities. The script uses relative `DATA_DIR="./flowio-logs"`, so it should be launched from `Capture_Flow.io_Logs/`.

**Windows:** PowerShell 5.1 or 7+, using built-in Web session and file APIs. The output directory is relative to the script file via `$PSScriptRoot`.

Both scripts expose these editable values: device IP/port (`FLOWIO_IP`), Web username (`FLOWIO_LOGIN`), password (`FLOWIO_PASSWORD`), polling interval (`POLL_SECONDS`) and output directory (`DATA_DIR`).

The distributed scripts use `admin` / `flowio1234` as **example factory credentials**. Always replace them if the device password has been changed, and do not commit customized passwords.

### 6. Safety and known limitations

- Collectors run on the **operator's computer**, not on the ESP32-S3. They do not change device configuration or restart the device.
- The current scripts use **HTTP** and store configured credentials as plain text. Run only on a trusted local network, protect local file permissions and avoid publishing edited scripts, cookies or collected logs.
- Runtime logs are bounded by Flow.io's memory history: if the ring buffer overwrites old entries before polling, the collector cannot recreate those entries; it can only report the loss.
- The runtime log file is **append-only** and has no automatic rotation/retention limit. Operators should monitor free disk space and archive/rotate logs themselves when necessary.
- A complete boot snapshot must still be available from the device when the collector requests it. The script cannot retrieve a boot capture that the firmware no longer exposes.
- The repository change is a **tooling/documentation addition**, not proof of a Linux/Windows live end-to-end collection test. The existing scripts were reviewed for their API calls and persistence behavior.

### 7. Change isolation / pull request review

- Base: `christophebelmont/Flow.io-Waveshare:dev`.
- Source: `stephdust/Flow.io-Waveshare:docs/capture-flowio-logs-20261124`.
- Only files inside `Capture_Flow.io_Logs/` are added.
- No overlap with the four firmware PRs (#7 through #10) or the separate combined sanity-check compilation branch.
- Expected firmware/runtime impact: **none**, because the collector is launched manually on a separate Linux/Windows computer.

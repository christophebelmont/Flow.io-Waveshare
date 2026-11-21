#!/usr/bin/env python3
"""Static Flow.io OTA HTTP server with a legacy SPIFFS-first safety interlock.

Run from the checkout:
    python scripts/serve_ota_guarded.py --port 90 --root .

The OLD updater accepts /binary/flowios3-spiffs-VERSION.bin and erases the
active filesystem after HTTP 200. This server returns 403 for that filename,
before any transfer. The NEW A/B updater downloads its identical SPIFFS image
through /binary/flowio-paired-spiffs-VERSION.bin as specified in the paired
flowio-release-VERSION.json manifest.

Protection applies only while this server is the one actually serving the
images. Existing firmware cannot be patched retrospectively.
"""
from __future__ import annotations

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit


_LEGACY_UNSAFE_SPIFFS = re.compile(
    r"^(?:flowios3|esp32s3|waveshare)-spiffs-[0-9][0-9a-z._-]*\.bin$",
    re.IGNORECASE,
)


def is_unsafe_legacy_spiffs_url(request_target: str) -> bool:
    """Return True if an old standalone SPIFFS image is requested."""
    path = unquote(urlsplit(request_target).path)
    basename = path.rsplit("/", 1)[-1]
    return bool(_LEGACY_UNSAFE_SPIFFS.fullmatch(basename))


class SafeOtaHandler(SimpleHTTPRequestHandler):
    def _reject_unsafe_legacy_spiffs(self) -> bool:
        if not is_unsafe_legacy_spiffs_url(self.path):
            return False
        self.log_error("[OTA-GUARD] BLOCKED unsafe standalone SPIFFS download: %s", self.path)
        self.send_error(
            403,
            "Direct legacy SPIFFS OTA blocked. Install firmware first; "
            "the new paired A/B updater will use the safe image URL.",
        )
        return True

    def do_GET(self) -> None:
        if not self._reject_unsafe_legacy_spiffs():
            super().do_GET()

    def do_HEAD(self) -> None:
        if not self._reject_unsafe_legacy_spiffs():
            super().do_HEAD()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="directory containing binary/ (default: cwd)")
    parser.add_argument("--bind", default="0.0.0.0", help="listening address for devices on LAN")
    parser.add_argument("--port", type=int, default=90)
    args = parser.parse_args()

    root = Path(args.root).expanduser().resolve(strict=True)
    if not (root / "binary").is_dir():
        parser.error(f"binary/ directory not found under {root}")
    handler = partial(SafeOtaHandler, directory=str(root))
    server = ThreadingHTTPServer((args.bind, args.port), handler)
    print(f"[OTA-GUARD] Serving {root} at http://{args.bind}:{args.port}/")
    print("[OTA-GUARD] Old standalone SPIFFS URLs: HTTP 403")
    print("[OTA-GUARD] Paired A/B SPIFFS aliases: HTTP 200 (if present)")
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()

"""Exercise real guarded HTTP routes, not just filename matching.

Run: python -m unittest scripts.tests.test_ota_guarded_server
"""
from __future__ import annotations

from functools import partial
from http.server import ThreadingHTTPServer
import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[2]
GUARD_SCRIPT = ROOT / "scripts" / "serve_ota_guarded.py"
spec = importlib.util.spec_from_file_location("ota_guarded", GUARD_SCRIPT)
guarded = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(guarded)


class GuardedMigrationServerTests(unittest.TestCase):
    def test_legacy_direct_spiffs_denied_but_paired_alias_allowed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "binary"
            binary.mkdir()
            files = {
                "flowios3-2.0.1.bin": b"firmware",
                "flowios3-spiffs-2.0.1.bin": b"same-spiffs",
                "flowio-paired-spiffs-2.0.1.bin": b"same-spiffs",
                "flowio-release-2.0.1.json": b'{"filesystem":{"file":"flowio-paired-spiffs-2.0.1.bin"}}',
            }
            for name, data in files.items():
                (binary / name).write_bytes(data)

            handler = partial(guarded.SafeOtaHandler, directory=str(root))
            server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                base = f"http://127.0.0.1:{server.server_port}/binary/"
                for legacy in (
                    "flowios3-spiffs-2.0.1.bin",
                    "esp32s3-spiffs-2.0.1.bin",
                    "waveshare-spiffs-2.0.1.bin",
                    "flowios3-spiffs-2.0.1.bin?nocache=1",
                    "flowios3-spiffs-2.0.1%2Ebin",
                ):
                    for method in ("GET", "HEAD"):
                        with self.subTest(legacy=legacy, method=method):
                            with self.assertRaises(HTTPError) as caught:
                                urlopen(Request(base + legacy, method=method), timeout=3)
                            self.assertEqual(403, caught.exception.code)
                for filename in (
                    "flowios3-2.0.1.bin",
                    "flowio-paired-spiffs-2.0.1.bin",
                    "flowio-release-2.0.1.json",
                ):
                    for method in ("GET", "HEAD"):
                        with self.subTest(filename=filename, method=method):
                            with urlopen(Request(base + filename, method=method), timeout=3) as response:
                                self.assertEqual(200, response.status)
                                if method == "GET":
                                    self.assertEqual(files[filename], response.read())
                with urlopen(base + "flowio-release-2.0.1.json", timeout=3) as response:
                    document = json.load(response)
                self.assertEqual(
                    "flowio-paired-spiffs-2.0.1.bin",
                    document["filesystem"]["file"],
                )
            finally:
                server.shutdown()
                thread.join(timeout=3)
                server.server_close()


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""
Inject build reference macros into the active PlatformIO environment.

Stable product versions stay in platformio.ini (`custom_version` / `FIRMW`),
while this script appends a build reference generated at compile time.
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path
import sys

scripts_dir = str(Path.cwd() / "scripts")
if scripts_dir not in sys.path:
    sys.path.insert(0, scripts_dir)

from version_utils import normalize_config_string

Import("env")  # type: ignore[name-defined]


core_version = normalize_config_string(env.GetProjectOption("custom_version", "0.0.0"))
build_ref = datetime.now().strftime("%Y%m%d.%H%M%S")
full_version = f"{core_version}+{build_ref}"

env.Append(
    CPPDEFINES=[
        ("FLOW_BUILD_REF", f'\\"{build_ref}\\"'),
        ("FLOW_FIRMWARE_VERSION_FULL", f'\\"{full_version}\\"'),
    ]
)

print(f"[build-version] core={core_version} build_ref={build_ref} full={full_version}")

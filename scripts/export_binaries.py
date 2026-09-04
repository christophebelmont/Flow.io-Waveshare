from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil

Import("env")


_ARTIFACT_RE = re.compile(
    r"^(?P<software>[A-Za-z0-9][A-Za-z0-9._-]*)-(?P<version>[0-9][0-9A-Za-z._-]*)\.(?P<ext>bin|tft)$"
)
_VERSION_SANITIZE_RE = re.compile(r"[^0-9A-Za-z._-]+")
_NEXTION_FILENAME_RE = re.compile(
    r"^FlowIO_Nextion_(?P<model>NX[0-9]{4}[A-Za-z][0-9]{3}(?:[_-][0-9]{3}(?:[RCN])?(?:[_-][A-Za-z0-9]+)*)?)-"
    r"(?P<version>[0-9]+\.[0-9]+\.[0-9]+)\.tft$"
)
_NEXTION_MODEL_RE = re.compile(
    r"^(?P<base>NX[0-9]{4}[A-Za-z][0-9]{3})"
    r"(?:[_-](?P<revision>[0-9]{3})(?P<touch>[RCN])?(?P<suffix>(?:[_-][A-Za-z0-9]+)*))?$",
    re.IGNORECASE,
)


def _project_dir():
    return Path(env.subst("$PROJECT_DIR"))


def _binary_dir():
    out_dir = _project_dir() / "binary"
    out_dir.mkdir(exist_ok=True)
    return out_dir


def _clean_value(value):
    if value is None:
        return ""
    return str(value).strip().replace("\\", "").strip('"').strip("'")


def _sanitize_version(value):
    cleaned = _VERSION_SANITIZE_RE.sub("", _clean_value(value))
    return cleaned if cleaned else "0.0.0"


def _resolve_firmware_version():
    version = ""
    try:
        version = _sanitize_version(env.GetProjectOption("custom_version"))
    except Exception:
        version = ""

    if version:
        return version

    for define in env.get("CPPDEFINES", []):
        if isinstance(define, (tuple, list)) and len(define) >= 2 and define[0] == "FIRMW":
            version = _sanitize_version(define[1])
            if version:
                return version

    return "0.0.0"


def _resolve_define_version(name):
    for define in env.get("CPPDEFINES", []):
        if isinstance(define, (tuple, list)) and len(define) >= 2 and define[0] == name:
            return _sanitize_version(define[1])
    return "0.0.0"


def _normalize_nextion_compatibility(model):
    match = _NEXTION_MODEL_RE.fullmatch(str(model or "").strip())
    if not match:
        raise ValueError(f"modèle Nextion invalide: {model}")
    touch = (match.group("touch") or "").upper()
    if touch in ("R", "C"):
        raise ValueError("le nom d'un artefact Nextion doit omettre le marqueur tactile R/C")

    compatibility = match.group("base").upper()
    revision = match.group("revision")
    if revision:
        compatibility += "_" + revision
        if touch == "N":
            compatibility += "N"
        suffix = match.group("suffix") or ""
        if suffix:
            compatibility += re.sub(r"[_-]", "_", suffix.upper())
    return compatibility


def _parse_nextion_filename(filename):
    match = _NEXTION_FILENAME_RE.fullmatch(str(filename or ""))
    if not match:
        raise ValueError(
            "format attendu: FlowIO_Nextion_<MODELE_COMPATIBLE>-<VERSION>.tft"
        )
    return {
        "display_compatibility": _normalize_nextion_compatibility(match.group("model")),
        "version": match.group("version"),
    }


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _classify_artifact(software, ext, filename):
    software_name = str(software or "").strip()
    norm = software_name.lower()

    if ext == "tft":
        nextion = _parse_nextion_filename(filename)
        return {
            "category": "nextion",
            "target": "nextion",
            "kind": "nextion-tft",
            "route": "/fwupdate/nextion",
            "display_compatibility": nextion["display_compatibility"],
            "version": nextion["version"],
        }
    if norm in ("flowios3-spiffs", "esp32s3-spiffs", "waveshare-spiffs"):
        return {
            "category": "spiffs",
            "target": "spiffs",
            "kind": "esp32-spiffs",
            "route": "/fwupdate/spiffs",
        }
    if norm in ("flowios3", "esp32s3", "waveshare"):
        return {
            "category": "flowios3",
            "target": "flowios3",
            "kind": "esp32-firmware",
            "route": "/fwupdate/waveshare",
        }

    return None


def _update_manifest():
    out_dir = _binary_dir()
    now_iso = datetime.now().astimezone().isoformat(timespec="seconds")
    artifacts = {}
    newest_ts = None

    for path in sorted(out_dir.iterdir()):
        if not path.is_file():
            continue
        if path.name == "manifest.json":
            continue

        match = _ARTIFACT_RE.match(path.name)
        if not match:
            print(f"[export_binaries] skip manifest entry for '{path.name}' (format attendu: <software>-<version>.bin/tft)")
            continue

        stat = path.stat()
        mtime = datetime.fromtimestamp(stat.st_mtime).astimezone()
        mtime_iso = mtime.isoformat(timespec="seconds")
        if newest_ts is None or stat.st_mtime > newest_ts:
            newest_ts = stat.st_mtime

        software = match.group("software")
        version = match.group("version")
        ext = match.group("ext")
        try:
            spec = _classify_artifact(software, ext, path.name)
        except ValueError as exc:
            raise RuntimeError(f"artefact Nextion invalide '{path.name}': {exc}") from exc
        if spec is None:
            print(f"[export_binaries] skip manifest entry for '{path.name}'")
            continue

        if "version" in spec:
            version = spec["version"]
        entry = {
            "title": software,
            "label": software,
            "version": version,
            "build_date": mtime_iso,
            "target": spec["target"],
            "path": path.name,
            "kind": spec["kind"],
            "route": spec["route"],
            "size": stat.st_size,
            "sha256": _sha256(path),
        }
        if "display_compatibility" in spec:
            entry["display_compatibility"] = spec["display_compatibility"]
        artifacts.setdefault(spec["category"], []).append(entry)

    seen_nextion = set()
    for entry in artifacts.get("nextion", []):
        key = (entry["display_compatibility"], entry["version"])
        if key in seen_nextion:
            raise RuntimeError(
                "doublon Nextion dans le manifest: " + "/".join(key)
            )
        seen_nextion.add(key)

    manifest = {
        "schema": "flowio.firmware-manifest.v2",
        "generated_at": now_iso,
        "release": datetime.fromtimestamp(newest_ts).strftime("%Y.%m.%d") if newest_ts is not None else datetime.now().strftime("%Y.%m.%d"),
        "artifacts": artifacts,
    }

    manifest_path = out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    rel_manifest = manifest_path.relative_to(_project_dir())
    print(f"[export_binaries] manifest updated -> {rel_manifest}")


def _copy_if_exists(src_path, dst_name):
    src = Path(str(src_path))
    if not src.exists():
        return
    dst = _binary_dir() / dst_name
    shutil.copy2(src, dst)
    print(f"[export_binaries] copied {src.name} -> {dst.relative_to(_project_dir())}")
    _update_manifest()


def _export_nextion_release():
    version = _resolve_define_version("TFT_FIRMW")
    release_dir = _project_dir() / "nextion" / "releases" / version
    if not release_dir.is_dir():
        raise RuntimeError(f"release Nextion introuvable: {release_dir}")

    tft_files = sorted(release_dir.glob("*.tft"))
    if not tft_files:
        raise RuntimeError(f"aucun artefact TFT dans: {release_dir}")
    for src in tft_files:
        parsed = _parse_nextion_filename(src.name)
        if parsed["version"] != version:
            raise RuntimeError(
                f"version TFT incohérente pour '{src.name}': {parsed['version']} != {version}"
            )
        dst = _binary_dir() / src.name
        shutil.copy2(src, dst)
        print(f"[export_binaries] copied {src.name} -> {dst.relative_to(_project_dir())}")
    _update_manifest()


def _export_program_bin(source, target, env):
    build_dir = Path(env.subst("$BUILD_DIR"))
    env_name = env.subst("$PIOENV")
    fw_version = _resolve_firmware_version()

    if env_name == "Flowio-waveshare-esp32-s3":
        _copy_if_exists(build_dir / "firmware.bin", f"flowios3-{fw_version}.bin")
        _export_nextion_release()


def _export_spiffs_bin(source, target, env):
    build_dir = Path(env.subst("$BUILD_DIR"))
    env_name = env.subst("$PIOENV")
    fw_version = _resolve_firmware_version()
    if env_name == "Flowio-waveshare-esp32-s3":
        _copy_if_exists(build_dir / "spiffs.bin", f"flowios3-spiffs-{fw_version}.bin")
        return


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _export_program_bin)
env.AddPostAction("$BUILD_DIR/spiffs.bin", _export_spiffs_bin)

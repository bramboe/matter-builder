"""Writes manifest.json for the firmware images in a folder and signs it.

    MB_SIGNING_KEY=<PEM> python3 tools/release_manifest.py <folder> <release> [<source root>]

The manifest lists every update image (<project>-<version>.bin) with its
application, version, chip, size and SHA-256; "factory" lists the images for a
new ESP32 over USB (factory/<project>-<version>-factory.bin, bootloader,
partition table and app in one) the same way, and "packages" what each
firmware is, from firmware/*/package.yaml and packages/*/package.yaml of the
source root (this repository, or a package source's checkout). Factory images have a list of their
own so an add-on that does not know them never offers one as an update. The
signature
(manifest.json.sig) is ECDSA P-256 over SHA-256 of manifest.json, made with
openssl; the Home Assistant add-on verifies it with
ha-addon/matter_builder/release-key.pem.
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mb_generate import parse_yaml  # noqa: E402

PACKAGE_FIELDS = ("name", "app", "kind", "format", "title", "description")
# What the firmware needs from its board (e.g. bluetooth) and which networks it
# runs on; optional in package.yaml, like "settings" (the schema of what can be
# set on the device)
PACKAGE_DEFAULTS = {"requires": [], "network": ["thread"]}


def as_list(value):
    """A list from YAML: a block list, or the flow form "[a, b]" the small parser keeps as text."""
    if isinstance(value, list):
        return [str(v) for v in value]
    text = str(value).strip().strip("[]")
    return [v.strip() for v in text.split(",") if v.strip()]


def image(path, stem):
    app, _, version = stem.rpartition("-")
    data = path.read_bytes()
    return {"file": path.name, "app": app, "version": version, "chip": "esp32c6",
            "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def packages(apps, root):
    out = []
    for path in sorted([*root.glob("firmware/*/package.yaml"), *root.glob("packages/*/package.yaml")]):
        package = parse_yaml(path.read_text())
        missing = [k for k in PACKAGE_FIELDS if k not in package]
        if missing:
            sys.exit(f"{path}: missing {', '.join(missing)}")
        if package["app"] in apps:
            out.append({**{k: package[k] for k in PACKAGE_FIELDS},
                        **{k: as_list(package.get(k, v)) for k, v in PACKAGE_DEFAULTS.items()},
                        **({"settings": package["settings"]} if isinstance(package.get("settings"), dict) else {})})
    return out


def main(folder, release, root=ROOT):
    folder = Path(folder)
    images = [image(p, p.stem) for p in sorted(folder.glob("*.bin"))]
    factory = [image(p, p.stem.removesuffix("-factory")) for p in sorted(folder.glob("factory/*-factory.bin"))]
    manifest = folder / "manifest.json"
    manifest.write_text(json.dumps({"release": release, "images": images, "factory": factory,
                                    "packages": packages({i["app"] for i in images}, Path(root))}, indent=2) + "\n")

    key = os.environ.get("MB_SIGNING_KEY", "")
    if not key:
        sys.exit("MB_SIGNING_KEY is not set: refusing to publish unsigned firmware")
    with tempfile.NamedTemporaryFile("w", suffix=".pem", delete=False) as f:
        f.write(key)
        key_file = f.name
    try:
        subprocess.run(["openssl", "dgst", "-sha256", "-sign", key_file, "-out", str(manifest) + ".sig",
                        str(manifest)], check=True)
    finally:
        os.unlink(key_file)
    print(manifest.read_text())


if __name__ == "__main__":
    main(*sys.argv[1:4])

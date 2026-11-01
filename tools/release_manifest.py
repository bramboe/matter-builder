"""Writes manifest.json for the firmware images in a folder and signs it.

    MB_SIGNING_KEY=<PEM> python3 tools/release_manifest.py <folder> <release>

The manifest lists every image (<project>-<version>.bin) with its
application, version, chip, size and SHA-256. The signature
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


def main(folder, release):
    folder = Path(folder)
    images = []
    for path in sorted(folder.glob("*.bin")):
        app, _, version = path.stem.rpartition("-")
        data = path.read_bytes()
        images.append({"file": path.name, "app": app, "version": version, "chip": "esp32c6",
                       "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    manifest = folder / "manifest.json"
    manifest.write_text(json.dumps({"release": release, "images": images}, indent=2) + "\n")

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
    print(json.dumps(images, indent=2))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

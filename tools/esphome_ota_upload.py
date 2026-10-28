#!/usr/bin/env python3
"""Upload an app image to a device that still runs ESPHome, over the network.

Uses ESPHome's own OTA client, so it works exactly like an ESPHome update:
the image goes into the free OTA slot and ESPHome stays in the other slot.

Needs the esphome package (e.g. a venv: python3 -m venv ~/esp/esphome-venv &&
~/esp/esphome-venv/bin/pip install esphome) and is run with that Python:

    ~/esp/esphome-venv/bin/python tools/esphome_ota_upload.py 192.168.1.50 \
        firmware/spike/build/octo_spike.bin

Set ESPHOME_OTA_PASSWORD if the device's ESPHome config has an OTA password.
"""

from __future__ import annotations

import logging
import os
import sys
from pathlib import Path

from esphome import espota2


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    host, image = sys.argv[1], Path(sys.argv[2])
    if image.read_bytes()[:1] != b"\xe9":
        print(f"{image} is not an ESP32 app image")
        return 1
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    rc, _ = espota2.run_ota(host, 3232, os.environ.get("ESPHOME_OTA_PASSWORD"), image)
    return rc


if __name__ == "__main__":
    sys.exit(main())

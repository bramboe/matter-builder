#!/bin/sh
# Matter Builder without Home Assistant or Docker (macOS, Linux): the same
# add-on as in Home Assistant, with a login of its own.
#
#   sh standalone/run.sh            then open http://localhost:8099
#
# Data (devices, settings, password, backups) goes to ~/.matter-builder,
# or to MB_DATA. Installing over USB from the page works in Chrome or Edge on
# this computer; esptool (pip) adds installing on the machine it runs on.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ADDON="$ROOT/ha-addon/matter_builder"
command -v python3 >/dev/null || { echo "Needs Python 3"; exit 1; }
python3 -c "import zeroconf" 2>/dev/null || echo "Tip: python3 -m pip install zeroconf  (finds devices on the network)"
python3 -c "import esptool" 2>/dev/null || echo "Tip: python3 -m pip install esptool  (installs over USB on this machine)"
MB_STANDALONE=1 MB_DATA="${MB_DATA:-$HOME/.matter-builder}" MB_SRC="$ADDON/src" \
  exec python3 -u "$ADDON/server.py"

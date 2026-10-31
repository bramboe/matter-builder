#!/bin/bash
# Sets up what the image does not hold, in $MB_DATA/toolchain: the ESP-IDF
# Python environment, esp-matter (fixed commit) and the parts of
# connectedhomeip the ESP32 build needs. Run once; safe to run again.
set -eo pipefail

DATA="${MB_DATA:-/data}"
COMMIT="${ESP_MATTER_COMMIT:?}"
DIR="$DATA/toolchain/esp-matter"

echo "== ESP-IDF Python environment ($IDF_PYTHON_ENV_PATH)"
mkdir -p "$IDF_PYTHON_ENV_PATH"
python3 /opt/esp/idf/tools/idf_tools.py install-python-env

mkdir -p "$DIR"
cd "$DIR"
[ -d .git ] || git init -q
git remote get-url origin > /dev/null 2>&1 || git remote add origin https://github.com/espressif/esp-matter.git

echo "== esp-matter $COMMIT"
git fetch --depth 1 origin "$COMMIT"
git checkout -q FETCH_HEAD

echo "== connectedhomeip (ESP32 and Linux parts only)"
git submodule update --init --depth 1 connectedhomeip/connectedhomeip
(cd connectedhomeip/connectedhomeip && ./scripts/checkout_submodules.py --platform esp32 linux --shallow)

echo "== Matter build environment (gn, zap)"
. /opt/esp/idf/export.sh > /dev/null
./install.sh --no-host-tool

rm -f "$DATA"/toolchain/ready-*
touch "$DATA/toolchain/ready-$COMMIT"
echo "== Toolchain ready"

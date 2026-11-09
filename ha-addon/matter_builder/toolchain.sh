#!/bin/bash
# Sets up what the image does not hold, in $MB_DATA/toolchain: the ESP-IDF
# Python environment, esp-matter (fixed commit) and the parts of
# connectedhomeip the ESP32 build needs. Run once; safe to run again.
set -eo pipefail

DATA="${MB_DATA:-/data}"
COMMIT="${ESP_MATTER_COMMIT:?}"
DIR="$DATA/toolchain/esp-matter"
. "$(dirname "$0")/idf_env.sh"

# ESP-IDF itself, when this image does not hold it (the add-on's light image):
# the firmware's version, with the compilers of the chips it supports
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "== ESP-IDF ${ESP_IDF_VERSION:-v5.5.5} into $IDF_PATH"
    rm -rf "$IDF_PATH"
    git clone -q --depth 1 --branch "${ESP_IDF_VERSION:-v5.5.5}" --recursive --shallow-submodules \
        https://github.com/espressif/esp-idf.git "$IDF_PATH"
fi
# A Python environment made by another Python (an older add-on image) cannot
# be reused: ESP-IDF removes it and then fails on the missing folder
if [ -x "$IDF_PYTHON_ENV_PATH/bin/python" ] && \
   [ "$("$IDF_PYTHON_ENV_PATH/bin/python" -c 'import sys; print(sys.version_info[:2])' 2>/dev/null)" != \
     "$(python3 -c 'import sys; print(sys.version_info[:2])')" ]; then
    echo "== Python environment from another Python: made again"
    rm -rf "$IDF_PYTHON_ENV_PATH"
fi

if [ "$IDF_PATH" != /opt/esp/idf ]; then
    # The compilers and the Python environment in one go
    echo "== ESP-IDF tools ($IDF_TOOLS_PATH) and Python environment ($IDF_PYTHON_ENV_PATH)"
    mkdir -p "$IDF_PYTHON_ENV_PATH"  # ESP-IDF lists it before it creates it
    "$IDF_PATH/install.sh" esp32c6,esp32 > /dev/null
else
    echo "== ESP-IDF Python environment ($IDF_PYTHON_ENV_PATH)"
    mkdir -p "$IDF_PYTHON_ENV_PATH"
    python3 "$IDF_PATH/tools/idf_tools.py" install-python-env
fi

mkdir -p "$DIR"
cd "$DIR"
[ -d .git ] || git init -q
git remote get-url origin > /dev/null 2>&1 || git remote add origin https://github.com/espressif/esp-matter.git

# The firmware sources, when the image does not hold them (an add-on
# installed from GitHub carries only the generator and the examples)
if [ ! -d /opt/octo-matter/components ] && [ ! -d "${MB_SRC:-/nonexistent}/components" ]; then
    echo "== Firmware sources into $DATA/sources"
    rm -rf "$DATA/sources"
    git clone -q --depth 1 "${MB_SOURCES_REPO:-https://github.com/bramboe/matter-builder.git}" "$DATA/sources"
fi

echo "== esp-matter $COMMIT"
git fetch --depth 1 origin "$COMMIT"
git checkout -q FETCH_HEAD

echo "== connectedhomeip (ESP32 and Linux parts only)"
git submodule update --init --depth 1 connectedhomeip/connectedhomeip
(cd connectedhomeip/connectedhomeip && ./scripts/checkout_submodules.py --platform esp32 linux --shallow)

echo "== Matter build environment (gn, zap)"
. "$IDF_PATH/export.sh" > /dev/null
./install.sh --no-host-tool

rm -f "$DATA"/toolchain/ready-*
touch "$DATA/toolchain/ready-$COMMIT"
echo "== Toolchain ready"

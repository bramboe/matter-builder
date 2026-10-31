#!/bin/bash
# Builds the Octo bridge for the ESP32-C6 from the sources in the image.
# Nothing user-specific is built in: Wi-Fi, Thread and the password are set
# on the bridge's own page. The image lands in $MB_DATA/firmware.
set -eo pipefail

DATA="${MB_DATA:-/data}"
SRC="${MB_SRC:-/opt/octo-matter}"
WORK="$DATA/work/octo-matter"
BUILD="$DATA/build/esp32c6"
export CCACHE_DIR="$DATA/ccache"

. /opt/esp/idf/export.sh > /dev/null
. "$DATA/toolchain/esp-matter/export.sh"

# A copy that keeps the image's timestamps, so unchanged files are not rebuilt
rm -rf "$WORK"
mkdir -p "$WORK" "$BUILD" "$DATA/firmware"
cp -a "$SRC/." "$WORK/"
cp "$WORK/firmware/bridge/main/secrets.example.h" "$WORK/firmware/bridge/main/secrets.h"

VERSION=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' "$WORK/firmware/bridge/CMakeLists.txt")
echo "== Octo bridge $VERSION for the ESP32-C6"
# A fresh configuration from the defaults every time, so a new version number
# or changed default always comes through (the cache keeps this fast)
rm -f "$BUILD/sdkconfig"
idf.py -C "$WORK/firmware/bridge" -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" -D IDF_TARGET=esp32c6 build

cp "$BUILD/octo_bridge.bin" "$DATA/firmware/octo_bridge-$VERSION.bin"
echo "== Ready: octo_bridge-$VERSION.bin ($(stat -c %s "$DATA/firmware/octo_bridge-$VERSION.bin") bytes)"

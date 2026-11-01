#!/bin/bash
# Builds one application for the ESP32-C6 from the sources in the image:
#   build.sh bridge    the Octo bed bridge  (octo_bridge-<version>.bin)
#   build.sh generic   the generic device   (mb_generic-<version>.bin)
# Nothing user-specific is built in: Wi-Fi, Thread, the password and what is
# connected are set on the device's own page. Images land in $MB_DATA/firmware.
set -eo pipefail

APP="${1:-bridge}"
DATA="${MB_DATA:-/data}"
SRC="${MB_SRC:-/opt/octo-matter}"
WORK="$DATA/work/octo-matter"
BUILD="$DATA/build/$APP-esp32c6"
export CCACHE_DIR="$DATA/ccache"

case "$APP" in
    bridge) TITLE="Octo bridge" ;;
    generic) TITLE="Generic device" ;;
    *) echo "unknown application: $APP"; exit 2 ;;
esac

. /opt/esp/idf/export.sh > /dev/null
. "$DATA/toolchain/esp-matter/export.sh"

# A copy that keeps the image's timestamps, so unchanged files are not rebuilt
rm -rf "$WORK"
mkdir -p "$WORK" "$BUILD" "$DATA/firmware"
cp -a "$SRC/." "$WORK/"
if [ -f "$WORK/firmware/$APP/main/secrets.example.h" ]; then
    cp "$WORK/firmware/$APP/main/secrets.example.h" "$WORK/firmware/$APP/main/secrets.h"
fi

PROJECT=$(sed -n 's/^project(\(.*\))/\1/p' "$WORK/firmware/$APP/CMakeLists.txt")
VERSION=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' "$WORK/firmware/$APP/CMakeLists.txt")
echo "== $TITLE $VERSION for the ESP32-C6"
# A fresh configuration from the defaults every time, so a new version number
# or changed default always comes through (the cache keeps this fast)
rm -f "$BUILD/sdkconfig"
idf.py -C "$WORK/firmware/$APP" -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" -D IDF_TARGET=esp32c6 build

cp "$BUILD/$PROJECT.bin" "$DATA/firmware/$PROJECT-$VERSION.bin"
echo "== Ready: $PROJECT-$VERSION.bin ($(stat -c %s "$DATA/firmware/$PROJECT-$VERSION.bin") bytes)"

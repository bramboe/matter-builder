#!/bin/bash
# Builds one application for the ESP32-C6 from the sources in the image:
#   build.sh generic         the generic device   (mb_generic-<version>.bin)
#   build.sh bridge          the Octo bed bridge  (octo_bridge-<version>.bin)
#   build.sh spec <file>     a device spec (tools/mb_generate.py): <name>-<version>.bin
#   build.sh project <dir>   an ESP-IDF project of a package source, on mb_core
#                            from these sources (MB_COMPONENTS): <project>-<version>.bin
# Each also gives factory/<project>-<version>-factory.bin for a new ESP32 (USB).
# Nothing user-specific is built in: Wi-Fi, Thread, the password and what is
# connected are set on the device's own page. Images land in $MB_DATA/firmware.
set -eo pipefail

APP="${1:-generic}"
DATA="${MB_DATA:-/data}"
SRC="${MB_SRC:-/opt/octo-matter}"
WORK="$DATA/work/octo-matter"
BUILD="$DATA/build/$APP-esp32c6"
export CCACHE_DIR="$DATA/ccache"

case "$APP" in
    generic) TITLE="Generic device" ;;
    bridge) TITLE="Octo bed bridge" ;;
    spec) TITLE="Device from spec"; SPEC_FILE="${2:?usage: build.sh spec <file>}" ;;
    project) TITLE="Package"; PROJECT_SRC="${2:?usage: build.sh project <dir>}" ;;
    *) echo "unknown application: $APP"; exit 2 ;;
esac

. /opt/esp/idf/export.sh > /dev/null
. "$DATA/toolchain/esp-matter/export.sh"

# A copy that keeps the image's timestamps, so unchanged files are not rebuilt
rm -rf "$WORK"
mkdir -p "$WORK" "$BUILD" "$DATA/firmware/factory"
cp -a "$SRC/." "$WORK/"
if [ -f "$WORK/firmware/$APP/main/secrets.example.h" ]; then
    cp "$WORK/firmware/$APP/main/secrets.example.h" "$WORK/firmware/$APP/main/secrets.h"
fi

PROJECT_DIR="$WORK/firmware/$APP"
export MB_COMPONENTS="$WORK/components"
if [ "$APP" = project ]; then
    PROJECT_DIR="$(cd "$PROJECT_SRC" && pwd)"
    BUILD="$DATA/build/project-$(basename "$PROJECT_DIR")"
    mkdir -p "$BUILD"
fi
if [ "$APP" = spec ]; then
    PROJECT_DIR="$DATA/work/spec"
    python3 "$WORK/tools/mb_generate.py" "$SPEC_FILE" "$PROJECT_DIR"
    SLUG=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["spec"]["slug"])' "$PROJECT_DIR/build-manifest.json")
    BUILD="$DATA/build/spec-$SLUG"
    mkdir -p "$BUILD"
fi
PROJECT=$(sed -n 's/^project(\(.*\))/\1/p' "$PROJECT_DIR/CMakeLists.txt")
VERSION=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' "$PROJECT_DIR/CMakeLists.txt")
echo "== $TITLE $VERSION for the ESP32-C6"
# A fresh configuration from the defaults every time, so a new version number
# or changed default always comes through (the cache keeps this fast)
rm -f "$BUILD/sdkconfig"
idf.py -C "$PROJECT_DIR" -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" -D IDF_TARGET=esp32c6 build

cp "$BUILD/$PROJECT.bin" "$DATA/firmware/$PROJECT-$VERSION.bin"
# Bootloader, partition table and app in one image, for a new ESP32 over USB
(cd "$BUILD" && python -m esptool --chip esp32c6 merge_bin -o "$DATA/firmware/factory/$PROJECT-$VERSION-factory.bin" @flash_args)
echo "== Factory image: $PROJECT-$VERSION-factory.bin"
echo "== Ready: $PROJECT-$VERSION.bin ($(stat -c %s "$DATA/firmware/$PROJECT-$VERSION.bin") bytes)"

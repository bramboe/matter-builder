#!/bin/sh
# Build the bridge for both boards and collect a release in dist/:
#
#   dist/octo-matter-<version>/
#     esp32/     classic ESP32, Matter over Wi-Fi
#     esp32c6/   ESP32-C6, Matter over Thread
#     octo-matter-<version>-source.tar.gz
#     SHA256SUMS
#     INSTALL.md
#
# The images contain firmware/bridge/main/secrets.h (Wi-Fi password, Thread
# network key, HTTP password): keep the release to yourself. dist/ is not in
# git.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BRIDGE="$ROOT/firmware/bridge"
VERSION=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' "$BRIDGE/CMakeLists.txt")
OUT="$ROOT/dist/octo-matter-$VERSION"

. "$HOME/esp/esp-idf-v5.5.5/export.sh" >/dev/null
. "$HOME/esp/esp-matter/export.sh" >/dev/null

build() {  # <target> <build dir> <sdkconfig>
    if [ ! -f "$BRIDGE/$3" ]; then
        (cd "$BRIDGE" && idf.py -B "$2" -D SDKCONFIG="$3" set-target "$1")
    fi
    (cd "$BRIDGE" && idf.py -B "$2" -D SDKCONFIG="$3" build)
    mkdir -p "$OUT/$1"
    cp "$BRIDGE/$2/bootloader/bootloader.bin" \
       "$BRIDGE/$2/partition_table/partition-table.bin" \
       "$BRIDGE/$2/ota_data_initial.bin" \
       "$BRIDGE/$2/octo_bridge.bin" \
       "$BRIDGE/$2/flasher_args.json" "$OUT/$1/"
}

rm -rf "$OUT"
build esp32 build sdkconfig
build esp32c6 build-c6 sdkconfig.c6
git -C "$ROOT" archive --format=tar.gz --prefix="octo-matter-$VERSION/" \
    -o "$OUT/octo-matter-$VERSION-source.tar.gz" HEAD
cp "$ROOT/docs/install.md" "$OUT/INSTALL.md"
(cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS)
echo "Release in $OUT"

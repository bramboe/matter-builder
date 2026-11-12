#!/bin/bash
# Builds variants of the generic firmware locally, each in its own folder:
#   tools/build_generic.sh [thread] [wifi] [esp32]   (default: all three)
# thread: ESP32-C6 on Thread (build-thread), wifi: ESP32-C6 on Wi-Fi
# (build-wifi), esp32: the classic ESP32 on Wi-Fi (build-esp32). Expects
# ESP-IDF and esp-matter in ~/esp (or IDF_PATH / ESP_MATTER_PATH set).
cd "$(dirname "$0")/../firmware/generic" || exit 1
. "${IDF_PATH:-$HOME/esp/esp-idf-v5.5.5}/export.sh" > /dev/null 2>&1
export ESP_MATTER_PATH="${ESP_MATTER_PATH:-$HOME/esp/esp-matter}"
. "$ESP_MATTER_PATH/export.sh" > /dev/null 2>&1
status=0
build() {
    local dir="$1" chip="$2" net="$3"
    # A fresh configuration from the defaults, so changed defaults come through
    [ -f "$dir/sdkconfig" ] && mv "$dir/sdkconfig" "$dir/sdkconfig.old"
    idf.py -B "$dir" -D SDKCONFIG="$dir/sdkconfig" -D IDF_TARGET="$chip" -D MB_NETWORK="$net" build > "$dir.log" 2>&1
    local rc=$?
    echo "$dir: exit $rc"
    grep -m3 -E "error:|overflowed" "$dir.log"
    grep "binary size" "$dir.log" | tail -1
    [ $rc -eq 0 ] || status=1
}
for v in "${@:-thread wifi esp32}"; do
    for w in $v; do
        case "$w" in
            thread) build build-thread esp32c6 thread ;;
            wifi) build build-wifi esp32c6 wifi ;;
            esp32) build build-esp32 esp32 wifi ;;
            esp32s3|esp32c3|esp32c5) build "build-$w" "$w" wifi ;;
            *) echo "unknown variant: $w"; status=2 ;;
        esac
    done
done
exit $status

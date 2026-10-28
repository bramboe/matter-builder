#!/bin/sh
# Put the ESPHome Bluetooth proxy back exactly as it was: writes the full
# flash backup made by backup_flash.sh (bootloader, partitions, ESPHome app,
# Wi-Fi settings, everything).
#
# Usage: tools/restore_flash.sh <serial-port> <backup-dir>
set -eu

PORT="${1:?usage: $0 <serial-port> <backup-dir>}"
DIR="${2:?usage: $0 <serial-port> <backup-dir>}"
IMAGE="$DIR/esphome-full-flash.bin"

(cd "$DIR" && shasum -a 256 -c SHA256SUMS)

printf "Overwrite the complete flash on %s with %s? [y/N] " "$PORT" "$IMAGE"
read -r answer
[ "$answer" = "y" ] || { echo "Aborted"; exit 1; }

esptool --port "$PORT" --baud 460800 write-flash 0 "$IMAGE"
echo "Done. The device boots ESPHome again and reconnects to Home Assistant."

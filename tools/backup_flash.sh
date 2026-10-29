#!/bin/sh
# Make a full backup of the ESP32's flash (the ESPHome Bluetooth proxy) before
# it is flashed with octo-matter firmware. The flash is read twice and the two
# reads must be identical.
#
# The dump contains the Wi-Fi password and the ESPHome API key: it is written
# OUTSIDE this repository and must never be committed.
#
# Usage: tools/backup_flash.sh <serial-port> [backup-dir]
#   e.g. tools/backup_flash.sh /dev/cu.usbserial-0001
set -eu

PORT="${1:?usage: $0 <serial-port> [backup-dir]}"
BASE="${2:-$HOME/Documents/octo-esphome-backup}"
DIR="$BASE/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$DIR"
chmod 700 "$BASE" "$DIR"

echo "Backup to $DIR"
esptool --port "$PORT" chip-id | tee "$DIR/chip-id.txt"
esptool --port "$PORT" flash-id | tee "$DIR/flash-id.txt"

esptool --port "$PORT" --baud "${BAUD:-460800}" read-flash 0 ALL "$DIR/flash-read1.bin"
esptool --port "$PORT" --baud "${BAUD:-460800}" read-flash 0 ALL "$DIR/flash-read2.bin"

if ! cmp -s "$DIR/flash-read1.bin" "$DIR/flash-read2.bin"; then
    echo "ERROR: the two flash reads differ; do not flash this device, retry the backup" >&2
    exit 1
fi
mv "$DIR/flash-read1.bin" "$DIR/esphome-full-flash.bin"
rm "$DIR/flash-read2.bin"
(cd "$DIR" && shasum -a 256 esphome-full-flash.bin > SHA256SUMS)
chmod 600 "$DIR"/*

SIZE=$(wc -c < "$DIR/esphome-full-flash.bin" | tr -d ' ')
echo
echo "OK: $DIR/esphome-full-flash.bin ($SIZE bytes, verified by double read)"
echo "Restore with: tools/restore_flash.sh $PORT $DIR"

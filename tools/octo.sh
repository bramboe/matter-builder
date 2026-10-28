#!/bin/sh
# Talk to the spike over the network.
#
#   tools/octo.sh status
#   tools/octo.sh bed set 0 AA:BB:CC:DD:EE:01 0000 Links
#   tools/octo.sh --ota firmware/spike/build/octo_spike.bin
#   tools/octo.sh --ota esphome-firmware.ota.bin      (back to ESPHome)
#
# Host: $OCTO_HOST (default octo-matter.local). Token: from secrets.h.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${OCTO_HOST:-octo-matter.local}"
TOKEN=$(sed -n 's/^#define OCTO_HTTP_TOKEN "\(.*\)"/\1/p' "$ROOT/firmware/spike/main/secrets.h")

if [ "${1:-}" = "--ota" ]; then
    IMAGE="${2:?usage: $0 --ota <app image .bin>}"
    curl -sS --fail-with-body --max-time 300 -H "X-Octo-Token: $TOKEN" \
        --data-binary "@$IMAGE" "http://$HOST/ota"
    exit
fi

curl -sS --fail-with-body --max-time 20 -H "X-Octo-Token: $TOKEN" \
    --data-binary "${*:-help}" "http://$HOST/cmd"

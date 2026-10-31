#!/bin/sh
# Talk to the bridge over the network.
#
#   tools/octo.sh status
#   tools/octo.sh bed set 0 AA:BB:CC:DD:EE:01 0000 Links
#   tools/octo.sh --ota firmware/bridge/build/octo_bridge.bin
#   tools/octo.sh --ota esphome-firmware.ota.bin      (back to ESPHome)
#
# Host: $OCTO_HOST (default octo-matter.local; an IPv6 address in brackets,
# e.g. [fd00::1], for the bridge on Thread). Password: $OCTO_TOKEN (the one
# chosen on the settings page), else OCTO_HTTP_TOKEN from an older secrets.h.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${OCTO_HOST:-octo-matter.local}"
TOKEN="${OCTO_TOKEN:-$(sed -n 's/^#define OCTO_HTTP_TOKEN "\(.*\)"/\1/p' "$ROOT/firmware/bridge/main/secrets.h" 2>/dev/null)}"

if [ "${1:-}" = "--ota" ]; then
    IMAGE="${2:?usage: $0 --ota <app image .bin>}"
    curl -gsS --fail-with-body --max-time "${OCTO_OTA_TIMEOUT:-1800}" -H "X-Octo-Token: $TOKEN" \
        --data-binary "@$IMAGE" "http://$HOST/ota"
    exit
fi

curl -gsS --fail-with-body --max-time 20 -H "X-Octo-Token: $TOKEN" \
    --data-binary "${*:-help}" "http://$HOST/cmd"

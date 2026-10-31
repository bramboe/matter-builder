#!/bin/sh
# Talk to the bridge over the network.
#
#   tools/octo.sh status
#   tools/octo.sh bed set 0 AA:BB:CC:DD:EE:01 0000 Links
#   tools/octo.sh --ota firmware/bridge/build/octo_bridge.bin
#   tools/octo.sh --ota esphome-firmware.ota.bin      (back to ESPHome)
#   tools/octo.sh --link "My script"                  (a token of its own)
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

# Ask for a token of a program's own: needs the password, then a short press
# of the BOOT button (or three quick power-ons) within 2 minutes. The token
# is printed once; use it as OCTO_TOKEN.
if [ "${1:-}" = "--link" ]; then
    NAME="${2:?usage: $0 --link <program name>}"
    REPLY=$(curl -gsS --fail-with-body --max-time 20 -H "X-Octo-Token: $TOKEN" --data-binary "$NAME" "http://$HOST/link")
    ID=$(printf '%s' "$REPLY" | sed -n 's/.*"id":"\([0-9a-f]*\)".*/\1/p')
    [ -n "$ID" ] || { echo "$REPLY" >&2; exit 1; }
    echo "Press the BOOT button of the bridge within 2 minutes..." >&2
    while :; do
        STATE=$(curl -gsS --max-time 20 -H "X-Octo-Token: $TOKEN" -H "X-Octo-Link: $ID" "http://$HOST/link" || true)
        case "$STATE" in
            *'"linked"'*) printf '%s\n' "$STATE" | sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p'; exit 0 ;;
            *'"waiting"'*) sleep 3 ;;
            *) echo "$STATE" >&2; exit 1 ;;
        esac
    done
fi

curl -gsS --fail-with-body --max-time 20 -H "X-Octo-Token: $TOKEN" \
    --data-binary "${*:-help}" "http://$HOST/cmd"

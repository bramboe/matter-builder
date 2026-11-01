#!/bin/sh
# Copies the firmware sources the add-on builds into ha-addon/matter_builder/src
# (not in git), and optionally the whole add-on to Home Assistant's local
# add-on folder:
#
#   tools/addon_sync.sh                       sources only
#   tools/addon_sync.sh /Volumes/addons       also to the mounted Samba share
#
# secrets.h, build output and flash dumps are never copied.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADDON="$ROOT/ha-addon/matter_builder"

EXCLUDES="--exclude build/ --exclude build-*/ --exclude secrets.h --exclude sdkconfig --exclude sdkconfig.old
    --exclude managed_components/ --exclude dependencies.lock --exclude *.log --exclude *.bin --exclude .DS_Store"
set -f  # the exclude patterns are not for the shell
mkdir -p "$ADDON/src/firmware"
# shellcheck disable=SC2086  # word splitting of the exclude list is intended
rsync -a --delete $EXCLUDES "$ROOT/components" "$ADDON/src/"
# shellcheck disable=SC2086
for app in bridge generic; do
    # shellcheck disable=SC2086
    rsync -a --delete $EXCLUDES "$ROOT/firmware/$app" "$ADDON/src/firmware/"
done
mkdir -p "$ADDON/src/tools"
cp "$ROOT/tools/mb_generate.py" "$ADDON/src/tools/"
# shellcheck disable=SC2086
rsync -a --delete $EXCLUDES "$ROOT/examples" "$ADDON/src/"
cp "$ROOT/LICENSE" "$ADDON/src/"
echo "sources: $(du -sh "$ADDON/src" | cut -f1)"

if [ $# -ge 1 ]; then
    TARGET="$1/matter_builder"
    mkdir -p "$TARGET"
    rsync -a --delete --exclude '__pycache__' "$ADDON/" "$TARGET/"
    echo "copied to $TARGET"
fi

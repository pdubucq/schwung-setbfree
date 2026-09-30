#!/usr/bin/env bash
# Copy the built module onto an Ableton Move over the network.
#
# Usage:  ./scripts/install.sh
#         DEVICE=ableton@move5 ./scripts/install.sh
#
# Assumes passwordless SSH to the device — enable SSH on the Move's
# development settings page and install your key.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

DEVICE="${DEVICE:-ableton@move.local}"
REMOTE="${REMOTE:-/data/UserData/schwung/modules/sound_generators}"

MODULE_DIR="$REPO_ROOT/dist/setBfree-organ"
[ -d "$MODULE_DIR" ] || { echo "FAIL: dist/setBfree-organ not found. Run ./scripts/build.sh first."; exit 1; }

# The host resolves the module by module.json's "dsp" field, so a directory
# missing either file loads as "dlopen failed" with no further explanation.
for f in dsp.so module.json help.json; do
    [ -f "$MODULE_DIR/$f" ] || { echo "FAIL: $MODULE_DIR/$f missing. Re-run ./scripts/build.sh."; exit 1; }
done

echo "=== installing to $DEVICE:$REMOTE ==="

ssh "$DEVICE" "mkdir -p '$REMOTE'"
scp -r "$MODULE_DIR" "$DEVICE:$REMOTE/"

# World-writable so the Module Store can replace the directory on a later
# update without a permission error.
ssh "$DEVICE" "chmod -R a+rw '$REMOTE/setBfree-organ'"

echo ""
echo "Installed to $REMOTE/setBfree-organ"
echo "Modules rescan on list — no restart needed."
echo ""
echo "To watch the device log while loading:"
echo "  ssh $DEVICE 'touch /data/UserData/schwung/debug_log_on'"
echo "  ssh $DEVICE 'tail -f /data/UserData/schwung/debug.log'"

#!/bin/sh
# 04-run.sh — run the port. Usage: 04-run.sh [game-dir]
# The runner takes the guest ELF as argv[1] and has no ISO support: it reads
# the unpacked disc files from its working directory (see 01-extract.sh).
set -eu
. "$(dirname "$0")/common.sh"
GAMEDIR="${1:-$GAMEDIR_DEFAULT}"
[ -f "$GAMEDIR/SLUS_203.18" ] || { echo "no ELF — run 01-extract.sh first" >&2; exit 1; }
# Run whichever was built last: the Release runner (03-build-runner.sh) or
# the dev build (maintainer/dev-build.sh), so a quick dev rebuild is what
# you play. Other build trees under _build are only a fallback.
# PS2X_RUNNER=<path> picks one explicitly.
REL="$GAMEDIR/_build/build/ps2xRuntime/ps2EntryRunner"
DEV="$GAMEDIR/$PS2X_DEV_BUILD_NAME/ps2xRuntime/ps2EntryRunner"
if [ -n "${PS2X_RUNNER:-}" ]; then
  RUNNER="$PS2X_RUNNER"
elif [ -f "$DEV" ] && { [ ! -f "$REL" ] || [ "$DEV" -nt "$REL" ]; }; then
  RUNNER="$DEV"
elif [ -f "$REL" ]; then
  RUNNER="$REL"
else
  RUNNER="$(find "$GAMEDIR/_build" -name ps2EntryRunner -type f 2>/dev/null | head -1 || true)"
fi
[ -n "$RUNNER" ] && [ -f "$RUNNER" ] || { echo "no runner built — run 03-build-runner.sh first" >&2; exit 1; }
echo "runner: $RUNNER" >&2
# The runtime resolves cdrom0:/host:/mc0: paths under its working directory,
# so run from the game dir (disc files live there; memory cards land there).
cd "$GAMEDIR"
exec "$RUNNER" "$GAMEDIR/SLUS_203.18"

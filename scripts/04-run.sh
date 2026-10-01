#!/bin/sh
# 04-run.sh — run the port. Usage: 04-run.sh [game-dir]
# The runner takes the guest ELF as argv[1] and has no ISO support: it reads
# the unpacked disc files from its working directory (see 01-extract.sh).
set -eu
. "$(dirname "$0")/common.sh"
GAMEDIR="${1:-$GAMEDIR_DEFAULT}"
[ -f "$GAMEDIR/SLUS_203.18" ] || { echo "no ELF — run 01-extract.sh first" >&2; exit 1; }
RUNNER="$(find "$GAMEDIR/_build" -name ps2EntryRunner -type f 2>/dev/null | head -1 || true)"
[ -n "$RUNNER" ] || { echo "no runner built — run 03-build-runner.sh first" >&2; exit 1; }
# The runtime resolves cdrom0:/host:/mc0: paths under its working directory,
# so run from the game dir (disc files live there; memory cards land there).
cd "$GAMEDIR"
exec "$RUNNER" "$GAMEDIR/SLUS_203.18"

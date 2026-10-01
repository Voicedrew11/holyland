#!/bin/sh
# 05-run.sh — run the port. Usage: 05-run.sh [game-dir]
# The runner takes the guest ELF as positional argv[1]; it has no CDVD/ISO
# support, so disc reads are unimplemented for now (expect the boot to stall
# where the game first touches the disc — that gap is future work).
set -eu
GAMEDIR="${1:-$HOME/.local/share/kfiv-pc}"
[ -f "$GAMEDIR/SLUS_203.18" ] || { echo "no ELF — run 01-extract.sh first" >&2; exit 1; }
RUNNER="$(find "$GAMEDIR/_build" -name ps2EntryRunner -type f 2>/dev/null | head -1 || true)"
[ -n "$RUNNER" ] || { echo "no runner built — run 04-build-runner.sh first" >&2; exit 1; }
# The runtime resolves cdrom0:/host:/mc0: paths under its working directory,
# so run from the game dir (disc files live there; memory cards land there).
cd "$GAMEDIR"
exec "$RUNNER" "$GAMEDIR/SLUS_203.18"

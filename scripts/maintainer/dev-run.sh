#!/bin/sh
# dev-run.sh — headless, scripted run of the game, for testing a change.
# Usage: dev-run.sh <out-dir> <exit-tick> [VAR=value ...]
# Runs the dev build (or $PS2X_RUNNER) with a hidden window in a private copy
# of the game dir (symlinks to the disc files, own memory cards), exits at
# vsync tick <exit-tick> (60 ticks = 1 s) and leaves in <out-dir>:
#   log.txt  stdout+stderr        final.png  last presented frame
#   frame_NNNNNN.png  with PS2X_DUMP_EVERY=<n>
#   ctrl     write commands here while it runs (see docs/contributing.md)
# Extra VAR=value go to the environment, e.g. PS2X_STATS=1 or
#   PS2X_INPUT="1100:START:10,1500:CROSS:10,1900:CROSS:10"   (into the game)
# The game dir is $KFIV_GAMEDIR or the default ~/.local/share/kfiv-pc.
set -eu
. "$(dirname "$0")/../common.sh"
OUT="${1:?usage: dev-run.sh <out-dir> <exit-tick> [VAR=value ...]}"
EXIT_TICK="${2:?usage: dev-run.sh <out-dir> <exit-tick> [VAR=value ...]}"
shift 2
GAMEDIR="${KFIV_GAMEDIR:-$GAMEDIR_DEFAULT}"
RUNNER="${PS2X_RUNNER:-$GAMEDIR/$PS2X_DEV_BUILD_NAME/ps2xRuntime/ps2EntryRunner}"
[ -x "$RUNNER" ] || { echo "no runner at $RUNNER — run maintainer/dev-build.sh" >&2; exit 1; }
[ -f "$GAMEDIR/SLUS_203.18" ] || { echo "no ELF — run 01-extract.sh first" >&2; exit 1; }
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
RG="$OUT/game"; rm -rf "$RG"; mkdir -p "$RG"
for f in "$GAMEDIR"/*; do
  b="$(basename "$f")"
  case "$b" in
    mc0|mc1) cp -r "$f" "$RG/" ;;                 # don't touch your saves
    _build|"$PS2X_DEV_BUILD_NAME"|output|*.log|*_log.txt) ;;
    *) ln -s "$f" "$RG/$b" ;;
  esac
done
cd "$RG"
rc=0
env PS2X_HIDDEN=1 PS2X_DUMP_DIR="$OUT" PS2X_EXIT_TICK="$EXIT_TICK" \
  PS2X_CTRL="$OUT/ctrl" "$@" \
  timeout -k 10 1800 "$RUNNER" "$RG/SLUS_203.18" >"$OUT/log.txt" 2>&1 || rc=$?
echo "exit=$rc  log=$OUT/log.txt  frames=$(ls "$OUT"/*.png 2>/dev/null | wc -l)"
exit "$rc"

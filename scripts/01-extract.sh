#!/bin/sh
# 01-extract.sh — pull the boot ELF out of your disc image.
# Usage: 01-extract.sh "<path-to-iso>" [game-dir]
# Default game dir: ~/.local/share/kfiv-pc
set -eu
HERE="$(dirname "$0")"
ISO="${1:?usage: 01-extract.sh \"<path-to-iso>\" [game-dir]}"
GAMEDIR="${2:-$HOME/.local/share/kfiv-pc}"
mkdir -p "$GAMEDIR"
python3 "$HERE/extract_elf.py" "$ISO" "$GAMEDIR"
# Record which ISO this game dir resolves to (git-ignored, repairable pointer).
printf 'iso.path=%s\n' "$ISO" > "$GAMEDIR/game.properties"
echo "game dir: $GAMEDIR"

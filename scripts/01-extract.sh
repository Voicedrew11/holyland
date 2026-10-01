#!/bin/sh
# 01-extract.sh — unpack your disc image into the game dir.
# Usage: 01-extract.sh "<path-to-iso>" [game-dir]
# Writes the whole disc tree (boot ELF, IOP modules, DATA/): the runtime reads
# loose files from the game dir. Verifies the boot ELF against the build the
# function map in kfiv/ was made for.
set -eu
. "$(dirname "$0")/common.sh"
ISO="${1:?usage: 01-extract.sh \"<path-to-iso>\" [game-dir]}"
GAMEDIR="${2:-$GAMEDIR_DEFAULT}"
mkdir -p "$GAMEDIR"
python3 "$KFIV_ROOT/scripts/extract_elf.py" --all "$ISO" "$GAMEDIR"
if ! (cd "$GAMEDIR" && sha256sum -c "$KFIV_ROOT/kfiv/SLUS_203.18.sha256"); then
  echo "error: SLUS_203.18 does not match the supported release (USA, SLUS-20318)." >&2
  echo "The function map in kfiv/ only applies to that exact build." >&2
  exit 1
fi
echo "game dir: $GAMEDIR"

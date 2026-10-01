#!/bin/sh
# 02-recompile.sh — run ps2_recomp on kfiv/config.toml with local paths.
# Usage: 02-recompile.sh [game-dir]
# Copies kfiv/config.toml to <game-dir>/config.toml, pointing input/output at
# the game dir and ghidra_output at the committed function map. Expect
# "errors: 0".
set -eu
. "$(dirname "$0")/common.sh"
GAMEDIR="${1:-$GAMEDIR_DEFAULT}"
[ -f "$GAMEDIR/SLUS_203.18" ] || { echo "missing $GAMEDIR/SLUS_203.18 — run 01-extract.sh first" >&2; exit 1; }
mkdir -p "$GAMEDIR/output"
python3 - "$KFIV_ROOT/kfiv" "$GAMEDIR" <<'PY'
import sys
kfiv, gamedir = sys.argv[1], sys.argv[2]
out = []
for line in open(f"{kfiv}/config.toml").read().splitlines(keepends=True):
    s = line.strip()
    if s.startswith("input ="):
        out.append(f'input = "{gamedir}/SLUS_203.18"\n')
    elif s.startswith("output ="):
        out.append(f'output = "{gamedir}/output/"\n')
    elif s.startswith("ghidra_output ="):
        out.append(f'ghidra_output = "{kfiv}/SLUS_203.18.functions.csv"\n')
    else:
        out.append(line)
open(f"{gamedir}/config.toml", "w").write("".join(out))
print(f"wrote {gamedir}/config.toml")
PY
"$PS2X_BUILD/ps2xRecomp/ps2_recomp" "$GAMEDIR/config.toml"

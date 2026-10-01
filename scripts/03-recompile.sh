#!/bin/sh
# 03-recompile.sh — run ps2_recomp on kfiv/config.toml with local paths.
# Usage: 03-recompile.sh [game-dir]
# Copies kfiv/config.toml to <game-dir>/config.toml, rewrites input/output
# to the game dir, and recompiles. Expect "errors: 0".
set -eu
HERE="$(dirname "$0")"
GAMEDIR="${1:-$HOME/.local/share/kfiv-pc}"
REPO="${PS2X_REPO:-$HOME/src/PS2Recomp}"
BUILD="${PS2X_BUILD:-$REPO-build}"
mkdir -p "$GAMEDIR/output"
python3 - "$HERE/../kfiv/config.toml" "$GAMEDIR" <<'EOF'
import sys
src, gamedir = sys.argv[1], sys.argv[2]
t = open(src).read()
out = []
for line in t.splitlines(keepends=True):
    s = line.strip()
    if s.startswith("input ="):
        out.append(f'input = "{gamedir}/SLUS_203.18"\n')
    elif s.startswith("output ="):
        out.append(f'output = "{gamedir}/output/"\n')
    elif s.startswith("ghidra_output =") and '""' in s:
        csv = f"{gamedir}/SLUS_203.18.functions.csv"
        import os
        out.append(f'ghidra_output = "{csv}"\n' if os.path.exists(csv) else line)
    else:
        out.append(line)
open(f"{gamedir}/config.toml", "w").write("".join(out))
print(f"wrote {gamedir}/config.toml")
EOF
"$BUILD/ps2xRecomp/ps2_recomp" "$GAMEDIR/config.toml"

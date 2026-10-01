#!/bin/sh
# 02-analyze.sh — run ps2_analyzer; merge SDK stubs into kfiv/config.toml.
# Usage: 02-analyze.sh [game-dir]
# Only the analyzer's `stubs` list carries over (its boundaries are
# unreliable on stripped ELFs). Ghidra owns boundaries (see docs/workflow.md).
set -eu
GAMEDIR="${1:-$HOME/.local/share/kfiv-pc}"
REPO="${PS2X_REPO:-$HOME/src/PS2Recomp}"
BUILD="${PS2X_BUILD:-$REPO-build}"
ELF="$GAMEDIR/SLUS_203.18"
[ -f "$ELF" ] || { echo "missing $ELF — run 01-extract.sh first" >&2; exit 1; }
"$BUILD/ps2xAnalyzer/ps2_analyzer" "$ELF" "$GAMEDIR/analyzer.toml"
echo "analyzer wrote $GAMEDIR/analyzer.toml"
echo "Next: merge its stubs into kfiv/config.toml (normalize leading '_' and"
echo "drop names with no runtime handler — see docs/workflow.md step 4)."

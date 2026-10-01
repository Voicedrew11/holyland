#!/bin/sh
# analyze.sh — MAINTAINERS ONLY. Run ps2_analyzer to refresh SDK stub names.
# Usage: analyze.sh [game-dir]
# End users don't need this: the merged result is committed in
# kfiv/config.toml. Only the analyzer's `stubs` list is useful (its function
# boundaries are unreliable on stripped ELFs; Ghidra owns those, see
# docs/maintainers.md).
set -eu
. "$(dirname "$0")/../common.sh"
GAMEDIR="${1:-$GAMEDIR_DEFAULT}"
ELF="$GAMEDIR/SLUS_203.18"
[ -f "$ELF" ] || { echo "missing $ELF — run 01-extract.sh first" >&2; exit 1; }
"$PS2X_BUILD/ps2xAnalyzer/ps2_analyzer" "$ELF" "$GAMEDIR/analyzer.toml"
echo "analyzer wrote $GAMEDIR/analyzer.toml"
echo "Next: merge its stubs into kfiv/config.toml (normalize leading '_' and"
echo "drop names with no runtime handler — see docs/maintainers.md)."

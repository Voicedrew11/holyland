# common.sh — shared settings, sourced by the other scripts. Override via env.
KFIV_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Where extracted game data, generated C++ and the runner build live.
GAMEDIR_DEFAULT="$HOME/.local/share/kfiv-pc"
# PS2Recomp checkout (analyzer, recompiler, runtime) and its build tree.
PS2X_REPO="${PS2X_REPO:-$HOME/src/PS2Recomp}"
PS2X_BUILD="${PS2X_BUILD:-$PS2X_REPO-build}"
PS2X_URL="${PS2X_URL:-https://github.com/ran-j/PS2Recomp.git}"
# Upstream commit this repo's config and patches are known to work with.
PS2X_REF="${PS2X_REF:-c5a9d02}"

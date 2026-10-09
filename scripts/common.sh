# common.sh — shared settings, sourced by the other scripts. Override via env.
KFIV_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# scripts/maintainer/*.sh sit one level deeper
[ -d "$KFIV_ROOT/kfiv" ] || KFIV_ROOT="$(cd "$KFIV_ROOT/.." && pwd)"
# Where extracted game data, generated C++ and the runner build live.
GAMEDIR_DEFAULT="$HOME/.local/share/kfiv-pc"
# PS2Recomp checkout (analyzer, recompiler, runtime) and its build tree.
PS2X_REPO="${PS2X_REPO:-$HOME/src/PS2Recomp}"
PS2X_BUILD="${PS2X_BUILD:-$PS2X_REPO-build}"
PS2X_URL="${PS2X_URL:-https://github.com/ran-j/PS2Recomp.git}"
# Upstream commit this repo's config and patches are known to work with.
PS2X_REF="${PS2X_REF:-c5a9d02}"
# Contributors: persistent PS2Recomp checkout holding the patch series as
# commits (made by maintainer/dev-setup.sh; 03-build-runner.sh never touches
# it), and its incremental build tree under the game dir.
PS2X_DEV="${PS2X_DEV:-$HOME/src/PS2Recomp-kfiv}"
PS2X_DEV_BUILD_NAME="dev-build"

#!/bin/sh
# 03-build-runner.sh — build ps2EntryRunner with the generated sources.
# Usage: 03-build-runner.sh [game-dir]
# Rebuilds <game-dir>/_build from scratch: deletes it, clones PS2Recomp at
# the pinned commit, applies
# patches/*.patch, drops the generated code into the slots the runtime's CMake expects, and builds the runner:
#   output/*.h   -> ps2xRuntime/include/   (PCH logic already looks for the
#                                           generated headers there)
#   output/*.cpp -> ps2xRuntime/src/runner/ (globbed into ps2EntryRunner;
#                 generated register_functions.cpp REPLACES the runtime's
#                 default table definition — compiling both would duplicate
#                 the table/base/end symbols, compiling neither leaves the
#                 table all-null and the boot stalls at the entry point)
# Requires X11 dev headers for raylib/GLFW (Fedora):
#   sudo dnf install libXinerama-devel libXrandr-devel libXcursor-devel libXi-devel
# Requires FFmpeg SDK (RPM Fusion build, to match ffmpeg-libs):
#   sudo dnf install ffmpeg-devel
# NOTE: ps2_runtime.h uses SSE4.1 intrinsics but upstream sets no x86 flags,
# so force -msse4.1 (any x86-64 CPU from the last ~15 years has it).
export CXXFLAGS="${CXXFLAGS:-} -msse4.1"
set -eu
. "$(dirname "$0")/common.sh"
GAMEDIR="${1:-$GAMEDIR_DEFAULT}"
[ -d "$GAMEDIR/output" ] || { echo "no $GAMEDIR/output — run 02-recompile.sh first" >&2; exit 1; }
# _build is disposable, but refuse to delete commits that exist nowhere else
# (an old dev checkout at _build/repo). Develop in maintainer/dev-setup.sh's
# checkout instead; KFIV_FORCE_CLEAN=1 deletes anyway.
if [ -d "$GAMEDIR/_build/repo/.git" ] && [ "${KFIV_FORCE_CLEAN:-0}" != 1 ] &&
   [ -n "$(git -C "$GAMEDIR/_build/repo" rev-list -n1 --branches --not --remotes)" ]; then
  echo "error: $GAMEDIR/_build/repo has local commits that would be deleted:" >&2
  git -C "$GAMEDIR/_build/repo" log --oneline --branches --not --remotes | head >&2
  echo "export them first (maintainer/export-patches.sh) or set KFIV_FORCE_CLEAN=1" >&2
  exit 1
fi
rm -rf "$GAMEDIR/_build"
git clone "$PS2X_REPO" "$GAMEDIR/_build/repo"
git -C "$GAMEDIR/_build/repo" checkout "$PS2X_REF"
git -C "$GAMEDIR/_build/repo" submodule update --init --recursive
python3 "$KFIV_ROOT/scripts/apply-patches.py" "$GAMEDIR/_build/repo" \
  --patch-dir "$KFIV_ROOT/patches" --base "$PS2X_REF"
cp "$GAMEDIR"/output/*.h "$GAMEDIR/_build/repo/ps2xRuntime/include/"
# find+cp: 28k files exceed the shell's max argument list for a plain glob
find "$GAMEDIR/output/" -maxdepth 1 -name '*.cpp' -exec cp -t "$GAMEDIR/_build/repo/ps2xRuntime/src/runner/" {} +
cmake -S "$GAMEDIR/_build/repo" -B "$GAMEDIR/_build/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF \
  -DPS2X_ENABLE_DEBUG_UI=OFF
# Link of 28k unity objects spills temp files; /tmp is a small tmpfs here,
# so redirect to the game dir (same filesystem, plenty of space).
mkdir -p "$GAMEDIR/_build/tmp"
TMPDIR="$GAMEDIR/_build/tmp" cmake --build "$GAMEDIR/_build/build" --config Release -j"$(nproc)" --target ps2EntryRunner
echo "runner: $GAMEDIR/_build/build/ps2xRuntime/ps2EntryRunner (path may vary — check build tree)"

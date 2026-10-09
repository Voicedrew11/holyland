#!/bin/sh
# dev-build.sh — incremental dev build of the runner from the dev checkout.
# Usage: dev-build.sh [game-dir]
# Copies the generated code from <game-dir>/output into $PS2X_DEV (only
# files that changed, so a rebuild stays incremental) and builds
# ps2EntryRunner in <game-dir>/dev-build: -O2, no LTO. First build takes
# minutes; after a runtime .cpp change it recompiles that file and relinks
# in seconds. Editing a header the game code includes (ps2xRuntime/include)
# recompiles all 28k generated files.
# Output: <game-dir>/dev-build/ps2xRuntime/ps2EntryRunner
export CXXFLAGS="${CXXFLAGS:-} -msse4.1"   # see 03-build-runner.sh
set -eu
. "$(dirname "$0")/../common.sh"
GAMEDIR="${1:-$GAMEDIR_DEFAULT}"
REPO="$PS2X_DEV"
BUILD="$GAMEDIR/$PS2X_DEV_BUILD_NAME"
[ -d "$REPO/.git" ] || { echo "no $REPO — run maintainer/dev-setup.sh first" >&2; exit 1; }
[ -d "$GAMEDIR/output" ] || { echo "no $GAMEDIR/output — run 02-recompile.sh first" >&2; exit 1; }
# Headers and the function table: copy when the content differs (the
# checkout's own register_functions.cpp is newer than the generated one, so
# a timestamp test would keep the upstream one and the boot would stall).
for f in "$GAMEDIR"/output/*.h "$GAMEDIR/output/register_functions.cpp"; do
  case "$f" in *.h) d=include ;; *) d=src/runner ;; esac
  cmp -s "$f" "$REPO/ps2xRuntime/$d/$(basename "$f")" || cp "$f" "$REPO/ps2xRuntime/$d/"
done
# The other ~28k files: only those newer than the copy (find+cp: too many
# for a plain glob).
find "$GAMEDIR/output/" -maxdepth 1 -name '*.cpp' ! -name register_functions.cpp \
  -exec cp -u -t "$REPO/ps2xRuntime/src/runner/" {} +
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  cmake -S "$REPO" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -DNDEBUG" \
    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -DNDEBUG" \
    -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF \
    -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF
fi
mkdir -p "$BUILD/tmp"
TMPDIR="$BUILD/tmp" cmake --build "$BUILD" -j"$(nproc)" --target ps2EntryRunner
echo "runner: $BUILD/ps2xRuntime/ps2EntryRunner"

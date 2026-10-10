#!/bin/sh
# dev-build.sh — incremental dev build of the runner from the dev checkout.
# Usage: dev-build.sh [game-dir]
# Copies the generated code from <game-dir>/output into $PS2X_DEV (only
# files whose content changed, so a rebuild stays incremental) and builds
# ps2EntryRunner in <game-dir>/dev-build: -O2, no LTO. First build takes
# minutes; after a runtime .cpp change it recompiles that file and relinks
# in seconds. Editing a header the game code includes (ps2xRuntime/include)
# recompiles all 28k generated files.
# Uses Ninja and mold when installed; ccache with KFIV_CCACHE=1 (see
# docs/contributing.md).
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
# The other ~28k files: also by content. 02-recompile.sh rewrites every file
# (new mtime, same content), so a timestamp test would rebuild everything.
rsync -c -d --exclude=register_functions.cpp --include='*.cpp' --exclude='*' \
  "$GAMEDIR/output/" "$REPO/ps2xRuntime/src/runner/"
# Lifted VU1 programs (vu1lift.py output in <game-dir>/vu1lift), same way.
mkdir -p "$REPO/ps2xRuntime/src/vu1lift"
if [ -d "$GAMEDIR/vu1lift" ]; then
  rsync -c -d --delete --include='*.cpp' --exclude='*' \
    "$GAMEDIR/vu1lift/" "$REPO/ps2xRuntime/src/vu1lift/"
else
  rm -f "$REPO/ps2xRuntime/src/vu1lift/"*.cpp
fi
# Ninja and mold are used when installed (mold: 0.1 s link instead of 0.8 s).
# KFIV_CCACHE=1 builds through ccache: a fresh tree, a reverted header edit
# or a switch back to an earlier branch then rebuilds in ~1 min instead of
# ~5. GCC's precompiled header is not byte-identical between rebuilds, which
# would make every unity batch miss, so the PCH is off with ccache; a full
# rebuild that misses the cache (e.g. a real header change) then takes ~2
# min longer.
GEN="Unix Makefiles"; command -v ninja >/dev/null && GEN=Ninja
LAUNCHER=""; PCH=ON
if [ "${KFIV_CCACHE:-0}" = 1 ]; then
  command -v ccache >/dev/null || { echo "KFIV_CCACHE=1 but ccache is not installed" >&2; exit 1; }
  LAUNCHER="env;CCACHE_DEPEND=1;CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime;ccache"
  PCH=OFF
fi
LINK=""; command -v mold >/dev/null && LINK="-fuse-ld=mold"
NATIVE_ELF=""; NATIVE_METADATA=""
case "${KFIV_VU_NATIVE:-1}" in
  1) NATIVE_ELF="$(cd "$GAMEDIR" && pwd)/SLUS_203.18"; NATIVE_METADATA="$KFIV_ROOT/kfiv/vu1-native.json" ;;
  0) ;;
  *) echo "KFIV_VU_NATIVE must be 0 or 1" >&2; exit 1 ;;
esac
KEY="$GEN|$LAUNCHER|$PCH|$LINK|$NATIVE_ELF|$NATIVE_METADATA"
# A build tree can't switch generators; it is build output only, so start over.
if [ -f "$BUILD/CMakeCache.txt" ] &&
   ! grep -qx "CMAKE_GENERATOR:INTERNAL=$GEN" "$BUILD/CMakeCache.txt"; then
  echo "generator is now $GEN: recreating $BUILD"
  rm -rf "$BUILD"
fi
if [ ! -f "$BUILD/CMakeCache.txt" ] || [ "$(cat "$BUILD/kfiv-config" 2>/dev/null)" != "$KEY" ]; then
  cmake -S "$REPO" -B "$BUILD" -G "$GEN" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -DNDEBUG" \
    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -DNDEBUG" \
    "-DCMAKE_C_COMPILER_LAUNCHER=$LAUNCHER" "-DCMAKE_CXX_COMPILER_LAUNCHER=$LAUNCHER" \
    "-DPS2X_ENABLE_SCCACHE=$([ -n "$LAUNCHER" ] && echo OFF || echo ON)" \
    "-DPS2X_ENABLE_RUNNER_PCH=$PCH" "-DCMAKE_EXE_LINKER_FLAGS=$LINK" \
    -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF \
    -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF \
    "-DPS2X_VU1_NATIVE_ELF=$NATIVE_ELF" "-DPS2X_VU1_NATIVE_METADATA=$NATIVE_METADATA"
  echo "$KEY" > "$BUILD/kfiv-config"
fi
mkdir -p "$BUILD/tmp"
TMPDIR="$BUILD/tmp" cmake --build "$BUILD" -j"$(nproc)" --target ps2EntryRunner
echo "runner: $BUILD/ps2xRuntime/ps2EntryRunner"

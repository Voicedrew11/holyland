#!/bin/sh
# 00-build-tools.sh — clone and build the PS2Recomp tools (once).
# Usage: 00-build-tools.sh
# Produces ps2_recomp (and ps2_analyzer) under $PS2X_BUILD. Respects
# PS2X_REPO / PS2X_BUILD / PS2X_REF (see common.sh).
set -eu
. "$(dirname "$0")/common.sh"
if [ ! -d "$PS2X_REPO/.git" ]; then
  git clone --recurse-submodules "$PS2X_URL" "$PS2X_REPO"
  git -C "$PS2X_REPO" checkout "$PS2X_REF"
  git -C "$PS2X_REPO" submodule update --init --recursive
else
  echo "using existing $PS2X_REPO ($(git -C "$PS2X_REPO" rev-parse --short HEAD); pinned: $PS2X_REF)"
fi
cmake -S "$PS2X_REPO" -B "$PS2X_BUILD" \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_RUNTIME=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$PS2X_BUILD" --config Release -j"$(nproc)"
echo "tools built in $PS2X_BUILD"

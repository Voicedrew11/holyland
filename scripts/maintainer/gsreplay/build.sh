#!/bin/sh
# build.sh — MAINTAINERS ONLY. Build gsreplay against a PS2Recomp checkout.
# Usage: build.sh <ps2recomp-checkout> <reference-rev>
# Compiles the checkout's current GS CPU backend plus, as the reference, the
# backend at <reference-rev> (renamed GSCpuBackendRef), so a trace recorded
# with PS2X_GS_RECORD can be replayed through both and compared:
#   ./gsreplay <trace> [--no-ref] [--no-new] [--max-ops N]
# Exit status 1 if VRAM (at every sync point) or transfer state differs.
set -eu
REPO="${1:?usage: build.sh <ps2recomp-checkout> <reference-rev>}"
REF="${2:?usage: build.sh <ps2recomp-checkout> <reference-rev>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${GSREPLAY_OUT:-$HERE/out}"
R="$REPO/ps2xRuntime"
mkdir -p "$OUT/ref"
git -C "$REPO" show "$REF:ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp" |
  sed 's/GSCpuBackend/GSCpuBackendRef/g; s#runtime/gs/gs_cpu_backend.h#gs_cpu_backend_ref.h#' > "$OUT/ref/gs_cpu_backend_ref.cpp"
git -C "$REPO" show "$REF:ps2xRuntime/include/runtime/gs/gs_cpu_backend.h" |
  sed 's/GSCpuBackend/GSCpuBackendRef/g' > "$OUT/ref/gs_cpu_backend_ref.h"
# Same flags as the runner's dev build (results depend on FP code generation).
c++ -std=gnu++20 -O2 -DNDEBUG -msse4.1 -I"$R/include" -I"$R/src/lib" -I"$R/src/lib/gs" -I"$OUT" \
  "$HERE/gsreplay.cpp" "$R/src/lib/gs/gs_cpu_backend.cpp" "$OUT/ref/gs_cpu_backend_ref.cpp" \
  "$R/src/lib/gs/ps2_gs_memory.cpp" -o "$OUT/gsreplay" -lpthread
echo "built $OUT/gsreplay (reference: $REF)"

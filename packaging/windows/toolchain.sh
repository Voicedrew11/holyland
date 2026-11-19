#!/usr/bin/env bash
# CONTENT/toolchain/ for Windows (docs/port/LAUNCHER.md, "The tools"), all from packaging/toolchain.lock:
#
#   llvm-mingw/  llvm-mingw (the developers' release, clang 22) with only its x86_64 target kept
#   cmake/       CMake's release; ninja/ninja.exe, Ninja's
#   SDL3/        SDL3's MinGW development build: the build links with it and copies SDL3.dll beside kf4.exe
#   ID           names these tools in the build key: change it whenever any of them changes
#
# Runs anywhere with bash, unzip and curl (CI: Git Bash on windows-latest; Linux works too).
#
#   packaging/windows/toolchain.sh CONTENT
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONTENT="${1:?usage: packaging/windows/toolchain.sh CONTENT}"
OUT="$CONTENT/toolchain"
ID="windows-2 llvm-mingw-20260616 cmake-3.31.6 ninja-1.12.1 sdl-3.4.20"
fetch() { "$ROOT/packaging/fetch.sh" "$1"; }

rm -rf "$OUT"
mkdir -p "$OUT" "$ROOT/out"
WORK="$(mktemp -d "$ROOT/out/toolchain-work.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

echo "==> llvm-mingw (x86_64 only)"
unzip -q "$(fetch llvm-mingw)" -d "$WORK"
LM="$WORK/llvm-mingw-20260616-ucrt-x86_64"
# its other targets (i686, armv7, aarch64): their libraries and their tools' names
rm -rf "$LM"/i686-w64-mingw32 "$LM"/armv7-w64-mingw32 "$LM"/aarch64-w64-mingw32 "$LM"/arm64ec-w64-mingw32 \
    "$LM"/lib/clang/*/lib/windows/*-i386.a "$LM"/lib/clang/*/lib/windows/*-armhf.a \
    "$LM"/lib/clang/*/lib/windows/*-aarch64.a "$LM"/lib/clang/*/lib/windows/*-arm64ec.a
find "$LM/bin" -maxdepth 1 \( -name 'i686-w64-*' -o -name 'armv7-w64-*' -o -name 'aarch64-w64-*' \
    -o -name 'arm64ec-w64-*' \) -delete
# what a build of the game never runs: the debugger, the editor tools, the sanitizer and profiling runtimes
rm -rf "$LM"/bin/clangd* "$LM"/bin/clang-tidy* "$LM"/bin/lldb* "$LM"/bin/liblldb* "$LM"/bin/*python* \
    "$LM"/bin/clang-format* "$LM"/share/man "$LM"/python "$LM"/share/doc "$LM"/lib/python* \
    "$LM"/x86_64-w64-mingw32/bin/libclang_rt.* "$LM"/x86_64-w64-mingw32/bin/libomp*
find "$LM"/lib/clang/*/lib/windows -type f ! -name 'libclang_rt.builtins-x86_64.a' -delete
mv "$LM" "$OUT/llvm-mingw"

echo "==> CMake, Ninja and SDL3"
unzip -q "$(fetch cmake-windows)" -d "$WORK"
mkdir -p "$OUT/cmake"
cp -r "$WORK/cmake-3.31.6-windows-x86_64/bin" "$WORK/cmake-3.31.6-windows-x86_64/share" "$OUT/cmake/"
rm -f "$OUT/cmake/bin/cmake-gui.exe"
rm -rf "$OUT/cmake/share/doc" "$OUT/cmake/share/cmake-3.31/Help" "$OUT/cmake/share/vim" "$OUT/cmake/share/emacs" \
    "$OUT/cmake/share/aclocal" "$OUT/cmake/share/bash-completion"
mkdir -p "$OUT/ninja"
unzip -q -o "$(fetch ninja-windows)" -d "$OUT/ninja"
unzip -q "$(fetch sdl3-mingw)" -d "$WORK"
mkdir -p "$OUT/SDL3"
cp -r "$WORK/SDL3-3.4.20/x86_64-w64-mingw32" "$WORK/SDL3-3.4.20/LICENSE.txt" "$OUT/SDL3/"
# (all of lib/ stays: SDL3's CMake files refuse to load with any of its libraries missing)
rm -rf "$OUT/SDL3/x86_64-w64-mingw32/share"

echo "$ID" > "$OUT/ID"
echo "done: $OUT ($(du -sh "$OUT" | cut -f1))"

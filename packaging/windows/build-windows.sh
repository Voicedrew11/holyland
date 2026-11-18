#!/usr/bin/env bash
# The Windows release (docs/port/LAUNCHER.md, "Releases"): dist/<NAME>-<version>-win-x64.zip and, where Inno
# Setup's iscc is on PATH, dist/<NAME>-<version>-win-x64-setup.exe (names from packaging/package.env). Both hold:
#
#   <APP_ID>.exe, SDL2.dll     the launcher (built here with the release's own llvm-mingw)
#   content\                   the game's source, the tools, manifest.txt
#
# Runs in Git Bash on Windows (the release workflow), or on Linux with LLVM_MINGW pointing at llvm-mingw's
# Linux build to cross-compile the launcher (no installer there). The game's tree
# needs its submodules (CLAUDE.md), not a configure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
. "$ROOT/packaging/package.env"
OUT="${OUT:-$ROOT/dist}"
STAGE="$OUT/win-x64"
VERSION="${VERSION:-$(bash "$ROOT/tools/version.sh")}"
CONTENT="$STAGE/content"

rm -rf "$STAGE"
mkdir -p "$STAGE"
echo "==> the game's source and the tools"
"$ROOT/packaging/payload.sh" "$CONTENT"
"$ROOT/packaging/windows/toolchain.sh" "$CONTENT"
T="$CONTENT/toolchain"

echo "==> the launcher, with the release's tools"
B="$ROOT/out/launcher-release-windows"
rm -rf "$B"
SDL2_DIR="$T/SDL2/x86_64-w64-mingw32/lib/cmake/SDL2"
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
    "$T/cmake/bin/cmake.exe" -G Ninja -S "$ROOT/launcher" -B "$B" -DCMAKE_MAKE_PROGRAM="$T/ninja/ninja.exe" \
        -DCMAKE_C_COMPILER="$T/llvm-mingw/bin/clang.exe" -DCMAKE_CXX_COMPILER="$T/llvm-mingw/bin/clang++.exe" \
        -DSDL2_DIR="$SDL2_DIR" -DCMAKE_BUILD_TYPE=Release -DHOLYLAND_VERSION="$VERSION" -DLAUNCHER_TESTS=OFF
    "$T/cmake/bin/cmake.exe" --build "$B"
    ;;
*)
    LM="${LLVM_MINGW:?on Linux, set LLVM_MINGW to the Linux build of llvm-mingw}"
    cmake -G Ninja -S "$ROOT/launcher" -B "$B" -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
        -DCMAKE_C_COMPILER="$LM/bin/x86_64-w64-mingw32-clang" -DCMAKE_CXX_COMPILER="$LM/bin/x86_64-w64-mingw32-clang++" \
        -DCMAKE_RC_COMPILER="$LM/bin/x86_64-w64-mingw32-windres" -DCMAKE_FIND_ROOT_PATH="$LM/x86_64-w64-mingw32" \
        -DSDL2_DIR="$SDL2_DIR" -DCMAKE_BUILD_TYPE=Release -DHOLYLAND_VERSION="$VERSION" -DLAUNCHER_TESTS=OFF
    cmake --build "$B"
    ;;
esac
cp "$B/$APP_ID.exe" "$STAGE/"
cp "$T/SDL2/x86_64-w64-mingw32/bin/SDL2.dll" "$STAGE/"
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
"$ROOT/packaging/manifest.sh" "$CONTENT"

echo "==> the zip"
ZIP="$OUT/$NAME-$VERSION-win-x64.zip"
rm -f "$ZIP"
if command -v 7z >/dev/null; then
    (cd "$STAGE" && 7z a -tzip -mx=9 "$ZIP" . >/dev/null)
else
    (cd "$STAGE" && zip -qr9 "$ZIP" .)
fi
echo "zip: $ZIP ($(du -h "$ZIP" | cut -f1))"

if command -v iscc >/dev/null; then
    echo "==> the installer"
    HOLYLAND_VERSION="$VERSION" HOLYLAND_NAME="$NAME" HOLYLAND_DISPLAY_NAME="$DISPLAY_NAME" \
        HOLYLAND_APP_ID="$APP_ID" HOLYLAND_INNO_APP_ID="$INNO_APP_ID" \
        HOLYLAND_INNO_PRIVILEGES="$INNO_PRIVILEGES" HOLYLAND_STAGE="$(cygpath -w "$STAGE" 2>/dev/null || echo "$STAGE")" \
        HOLYLAND_OUT="$(cygpath -w "$OUT" 2>/dev/null || echo "$OUT")" iscc "$(cygpath -w "$ROOT/packaging/windows/installer.iss" 2>/dev/null || echo "$ROOT/packaging/windows/installer.iss")"
fi

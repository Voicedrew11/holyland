#!/usr/bin/env bash
# The Linux release: dist/<NAME>-<version>-x86_64.AppImage (docs/port/LAUNCHER.md, "Releases"; names from
# packaging/package.env).
#
#   usr/bin/<APP_ID>                  the launcher (built here with the release's own tools)
#   usr/bin/libSDL3.so.0              its SDL3 (packaging/linux/build-sdl3.sh)
#   usr/share/<APP_ID>/content/       the game's source, the tools, manifest.txt
#
# It does NOT need the disc: the game is built on the player's computer. Run it on Ubuntu 22.04 (the release
# workflow does) or give an SDL3 built there: packaging/linux/build-appimage.sh [SDL3_PREFIX]. The game's tree
# needs its submodules (CLAUDE.md), not a configure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
. "$ROOT/packaging/package.env"
SDL3_PREFIX="${1:-}"
OUT="${OUT:-$ROOT/dist}"
APPDIR="$OUT/$NAME.AppDir"
VERSION="${VERSION:-$(bash "$ROOT/tools/version.sh")}"
CONTENT="$APPDIR/usr/share/$APP_ID/content"

chmod -R u+w "$APPDIR" 2>/dev/null || true
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

echo "==> the game's source and the tools"
"$ROOT/packaging/payload.sh" "$CONTENT"
# HOLYLAND_TOOLCHAIN_CACHE (the release workflow): a folder holding the tools toolchain.sh made before,
# taken as they are when it has them (unpacking LLVM and building SDL2 took 4 minutes there), else filled. Whoever
# sets it keys it on everything the tools are made from (toolchain.lock, toolchain.sh, build-sdl3.sh).
TOOLCHAIN_CACHE="${HOLYLAND_TOOLCHAIN_CACHE:-}"
if [ -n "$TOOLCHAIN_CACHE" ] && [ -f "$TOOLCHAIN_CACHE/ID" ]; then
    echo "==> the tools, from $TOOLCHAIN_CACHE ($(cat "$TOOLCHAIN_CACHE/ID"))"
    mkdir -p "$CONTENT"
    cp -a "$TOOLCHAIN_CACHE" "$CONTENT/toolchain"
else
    if [ -z "$SDL3_PREFIX" ]; then
        SDL3_PREFIX="$ROOT/out/sdl3-release"
        rm -rf "$SDL3_PREFIX"
        "$ROOT/packaging/linux/build-sdl3.sh" "$SDL3_PREFIX"
    fi
    "$ROOT/packaging/linux/toolchain.sh" "$CONTENT" "$SDL3_PREFIX"
    if [ -n "$TOOLCHAIN_CACHE" ]; then
        rm -rf "$TOOLCHAIN_CACHE"
        mkdir -p "$(dirname "$TOOLCHAIN_CACHE")"
        cp -a "$CONTENT/toolchain" "$TOOLCHAIN_CACHE"
    fi
fi

echo "==> the launcher, with the release's tools"
T="$CONTENT/toolchain"
B="$ROOT/out/launcher-release"
rm -rf "$B"
env -i HOME="$HOME" PATH="$T/llvm/bin:/usr/bin:/bin" "$T/cmake/bin/cmake" -G Ninja -S "$ROOT/launcher" -B "$B" \
    -DCMAKE_MAKE_PROGRAM="$T/bin/ninja" -DCMAKE_C_COMPILER="$T/llvm/bin/clang" \
    -DCMAKE_CXX_COMPILER="$T/llvm/bin/clang++" -DCMAKE_TOOLCHAIN_FILE="$T/linux.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DHOLYLAND_VERSION="$VERSION" -DLAUNCHER_TESTS=OFF
env -i HOME="$HOME" PATH="$T/llvm/bin:/usr/bin:/bin" "$T/cmake/bin/cmake" --build "$B"
cp "$B/$APP_ID" "$APPDIR/usr/bin/$APP_ID"
strip "$APPDIR/usr/bin/$APP_ID"
cp "$T/lib/libSDL3.so.0" "$APPDIR/usr/bin/"

"$ROOT/packaging/manifest.sh" "$CONTENT"
# the release's content is never written: the launcher builds into the player's builds folder
chmod -R a-w "$CONTENT"

# AppRun execs the launcher; the working folder is wherever the player started it, and $APPDIR is a read-only
# mount, so the launcher's data folder rule decides where files go
cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/@APP_ID@" "$@"
RUN
sed -i "s/@APP_ID@/$APP_ID/" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
for d in "$APPDIR/usr/share/applications/$APP_ID.desktop" "$APPDIR/$APP_ID.desktop"; do
    { cat "$ROOT/packaging/shared/$APP_ID.desktop"; echo "X-AppImage-Version=$VERSION"; } > "$d"
done
# no mark of its own yet: a transparent square, so appimagetool accepts the entry
ICON="$ROOT/packaging/shared/$APP_ID.png"
if [ ! -f "$ICON" ]; then
    ICON="$OUT/$APP_ID-blank.png"
    python3 - "$ICON" <<'PY'
import struct, sys, zlib
w = h = 256
raw = b"".join(b"\x00" + b"\x00\x00\x00\x00" * w for _ in range(h))
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + \
      chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
PY
fi
cp "$ICON" "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.png"
cp "$ICON" "$APPDIR/$APP_ID.png"
mkdir -p "$APPDIR/usr/share/doc/$APP_ID"
[ -f "$ROOT/LICENSE" ] && cp "$ROOT/LICENSE" "$APPDIR/usr/share/doc/$APP_ID/"

echo "==> appimagetool"
TOOL="$("$ROOT/packaging/fetch.sh" appimagetool)"
chmod +x "$TOOL"
ARCH=x86_64 "$TOOL" --no-appstream --comp zstd "$APPDIR" "$OUT/$NAME-$VERSION-x86_64.AppImage"
echo "done: $OUT/$NAME-$VERSION-x86_64.AppImage ($(du -h "$OUT/$NAME-$VERSION-x86_64.AppImage" | cut -f1))"

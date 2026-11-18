#!/usr/bin/env bash
# CONTENT/toolchain/ for Linux (docs/port/LAUNCHER.md, "The tools"): what the launcher builds the game with on
# the player's computer, all from packaging/toolchain.lock.
#
#   llvm/     clang, lld and llvm-ar from LLVM's release (as the developers' clang 22), stripped, and clang's
#             own headers: nothing else of LLVM's 2 GB
#   cmake/    CMake's release; bin/ninja, Ninja's
#   sysroot/  Ubuntu 22.04's C library, kernel headers, libstdc++ 12 (linked in statically), libdrm, and
#             SDL2's headers
#   lib/      libSDL2-2.0.so.0 (packaging/linux/build-sdl2.sh): the game links with it and it goes beside the
#             game (launcher/platform/linux.cpp: Toolchain::runtime_files)
#   linux.cmake  the CMake toolchain file that points the build at them
#   ID        names these tools in the build key: change it whenever any of them changes
#
#   packaging/linux/toolchain.sh CONTENT [SDL2_PREFIX]
#
# SDL2_PREFIX is build-sdl2.sh's output; without it, SDL2 is built here.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONTENT="${1:?usage: packaging/linux/toolchain.sh CONTENT [SDL2_PREFIX]}"
SDL2_PREFIX="${2:-}"
OUT="$CONTENT/toolchain"
ID="linux-1 llvm-22.1.8 cmake-3.31.6 ninja-1.12.1 ubuntu-22.04 sdl-2.30.9"
fetch() { "$ROOT/packaging/fetch.sh" "$1"; }

rm -rf "$OUT"
mkdir -p "$OUT/llvm/bin" "$OUT/bin" "$OUT/sysroot"
mkdir -p "$ROOT/out"
WORK="$(mktemp -d "$ROOT/out/toolchain-work.XXXXXX")" # (not /tmp: LLVM is 2 GB unpacked)
trap 'rm -rf "$WORK"' EXIT

echo "==> LLVM (clang, lld, llvm-ar)"
LLVM="LLVM-22.1.8-Linux-X64"
tar -xJf "$(fetch llvm-linux)" -C "$WORK" "$LLVM/bin/clang-22" "$LLVM/bin/lld" "$LLVM/bin/llvm-ar" \
    "$LLVM/lib/clang/22/include" "$LLVM/LICENSE.TXT" 2>/dev/null ||
    tar -xJf "$(fetch llvm-linux)" -C "$WORK" "$LLVM/bin/clang-22" "$LLVM/bin/lld" "$LLVM/bin/llvm-ar" \
        "$LLVM/lib/clang/22/include"
for tool in clang-22 lld llvm-ar; do
    cp "$WORK/$LLVM/bin/$tool" "$OUT/llvm/bin/"
    strip "$OUT/llvm/bin/$tool"
done
ln -s clang-22 "$OUT/llvm/bin/clang"
ln -s clang-22 "$OUT/llvm/bin/clang++"
ln -s lld "$OUT/llvm/bin/ld.lld"
ln -s llvm-ar "$OUT/llvm/bin/llvm-ranlib"
mkdir -p "$OUT/llvm/lib/clang/22"
cp -r "$WORK/$LLVM/lib/clang/22/include" "$OUT/llvm/lib/clang/22/"
[ -f "$WORK/$LLVM/LICENSE.TXT" ] && cp "$WORK/$LLVM/LICENSE.TXT" "$OUT/llvm/"

echo "==> CMake and Ninja"
tar -xzf "$(fetch cmake-linux)" -C "$WORK"
mkdir -p "$OUT/cmake"
cp -r "$WORK/cmake-3.31.6-linux-x86_64/bin" "$WORK/cmake-3.31.6-linux-x86_64/share" "$OUT/cmake/"
rm -f "$OUT/cmake/bin/cmake-gui" "$OUT/cmake/bin/ccmake"
rm -rf "$OUT/cmake/share/doc" "$OUT/cmake/share/cmake-3.31/Help" "$OUT/cmake/share/applications" \
    "$OUT/cmake/share/icons" "$OUT/cmake/share/mime" "$OUT/cmake/share/bash-completion" \
    "$OUT/cmake/share/emacs" "$OUT/cmake/share/vim" "$OUT/cmake/share/aclocal"
unzip -q -o "$(fetch ninja-linux)" -d "$OUT/bin"
chmod +x "$OUT/bin/ninja"

echo "==> the sysroot (Ubuntu 22.04)"
for deb in deb-libc6 deb-libc6-dev deb-linux-libc-dev deb-libgcc-12-dev deb-libgcc-s1 deb-libstdc++-12-dev \
    deb-libstdc++6 deb-libdrm-dev deb-libdrm2; do
    rm -rf "$WORK/deb" && mkdir -p "$WORK/deb"
    (cd "$WORK/deb" && ar x "$(fetch "$deb")")
    data="$(ls "$WORK/deb"/data.tar.*)"
    tar -xf "$data" -C "$OUT/sysroot"
done
S="$OUT/sysroot"
# what a build never reads
rm -rf "$S/usr/share" "$S/usr/bin" "$S/usr/sbin" "$S/sbin" "$S/etc" "$S/var" "$S/usr/libexec" \
    "$S/usr/lib/x86_64-linux-gnu/gconv" "$S/usr/lib/x86_64-linux-gnu/audit"
# links pointing at / are the sysroot's own files
find "$S" -type l | while read -r link; do
    target="$(readlink "$link")"
    case "$target" in
    /*) ln -sfn "$(realpath -m --relative-to="$(dirname "$link")" "$S$target")" "$link" ;;
    esac
done
echo "==> SDL2"
if [ -z "$SDL2_PREFIX" ]; then
    SDL2_PREFIX="$WORK/sdl2"
    "$ROOT/packaging/linux/build-sdl2.sh" "$SDL2_PREFIX"
fi
mkdir -p "$OUT/lib" "$S/usr/include"
cp -L "$SDL2_PREFIX/lib/libSDL2-2.0.so.0" "$OUT/lib/"
cp -r "$SDL2_PREFIX/include/SDL2" "$S/usr/include/"
# SDL_syswm.h (which Dear ImGui's SDL2 backend includes) brings in X11's and Wayland's headers for the window
# drivers SDL was built with. Nothing here reads those members (SDL_SysWMinfo's union is padded to the same
# size either way), so the drivers are left out of the header rather than X11 and Wayland put in the sysroot.
sed -i -E 's/^#define (SDL_VIDEO_DRIVER_(X11|WAYLAND|KMSDRM)[A-Z0-9_]*) 1$/\/* #undef \1 (holyland: packaging\/linux\/toolchain.sh) *\//' \
    "$S/usr/include/SDL2/SDL_config.h"
# SDL2 as the game's CMakeLists.txt finds it (find_package(SDL2 CONFIG))
mkdir -p "$S/usr/lib/cmake/SDL2"
cat > "$S/usr/lib/cmake/SDL2/SDL2Config.cmake" <<'CFG'
# SDL2 in the release's tools (packaging/linux/toolchain.sh): its headers in the sysroot, and the library
# shipped beside the game (toolchain/lib), which the game finds through its run path ($ORIGIN)
get_filename_component(_sysroot "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 SHARED IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${_sysroot}/../lib/libSDL2-2.0.so.0"
        IMPORTED_SONAME "libSDL2-2.0.so.0"
        INTERFACE_INCLUDE_DIRECTORIES "${_sysroot}/usr/include/SDL2"
        INTERFACE_COMPILE_DEFINITIONS "_REENTRANT")
endif()
set(SDL2_FOUND TRUE)
set(SDL2_VERSION 2.30.9)
CFG

echo "==> the toolchain file"
cat > "$OUT/linux.cmake" <<'CFG'
# The release's tools (packaging/linux/toolchain.sh): clang and lld on Ubuntu 22.04's sysroot, so the game
# runs on any Linux from then on. The launcher passes the compilers; this file adds the rest.
get_filename_component(_tools "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(CMAKE_SYSROOT "${_tools}/sysroot")
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_AR "${_tools}/llvm/bin/llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_tools}/llvm/bin/llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_C_COMPILER_AR "${_tools}/llvm/bin/llvm-ar" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER_AR "${_tools}/llvm/bin/llvm-ar" CACHE FILEPATH "")
set(CMAKE_C_COMPILER_RANLIB "${_tools}/llvm/bin/llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER_RANLIB "${_tools}/llvm/bin/llvm-ranlib" CACHE FILEPATH "")
# libstdc++ and libgcc inside the program: it needs only the C library, Vulkan and SDL2 from the system
# and SDL2 found beside it ($ORIGIN), never in the tools' folder
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -static-libstdc++ -static-libgcc -Wl,-rpath,\\$ORIGIN") # (\$: past the shell)
set(CMAKE_SKIP_BUILD_RPATH ON)
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -static-libstdc++ -static-libgcc")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_PREFIX_PATH "/usr/lib/cmake")
# no pkg-config: the build finds libdrm by its files (CMakeLists.txt)
set(PKG_CONFIG_EXECUTABLE "" CACHE FILEPATH "")
CFG
echo "$ID" > "$OUT/ID"
echo "done: $OUT ($(du -sh "$OUT" | cut -f1))"

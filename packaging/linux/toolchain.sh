#!/usr/bin/env bash
# CONTENT/toolchain/ for Linux (docs/port/LAUNCHER.md, "The tools"): what the launcher builds the game with on
# the player's computer, all from packaging/toolchain.lock.
#
#   llvm/     clang, lld and llvm-ar from LLVM's release (as the developers' clang 22), stripped, and clang's
#             own headers: nothing else of LLVM's 2 GB; llvm/lib/libxml2.so.2, a stand-in for the library lld
#             asks for and never uses (packaging/linux/libxml2-stub/)
#   cmake/    CMake's release; bin/ninja, Ninja's
#   sysroot/  Ubuntu 22.04's C library, kernel headers, libstdc++ 12 (linked in statically), libdrm, and
#             SDL3's headers
#   lib/      libSDL3.so.0 (packaging/linux/build-sdl3.sh): the game links with it and it goes beside the
#             game (launcher/platform/linux.cpp: Toolchain::runtime_files)
#   linux.cmake  the CMake toolchain file that points the build at them
#   ID        names these tools in the build key: change it whenever any of them changes
#
#   packaging/linux/toolchain.sh CONTENT [SDL3_PREFIX]
#
# SDL3_PREFIX is build-sdl3.sh's output; without it, SDL3 is built here.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONTENT="${1:?usage: packaging/linux/toolchain.sh CONTENT [SDL3_PREFIX]}"
SDL3_PREFIX="${2:-}"
OUT="$CONTENT/toolchain"
ID="linux-3 llvm-22.1.8 cmake-3.31.6 ninja-1.12.1 ubuntu-22.04 sdl-3.4.20"
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
# lld needs libxml2.so.2 to start, which Arch has only in libxml2-legacy (libxml2-stub/libxml2.c says why
# the stand-in is enough). Built with the host's compiler: it uses nothing, not even the C library.
mkdir -p "$OUT/llvm/lib"
cc -shared -fPIC -O2 -nostdlib -Wl,-soname,libxml2.so.2 \
    -Wl,--version-script,"$ROOT/packaging/linux/libxml2-stub/libxml2.ver" \
    -o "$OUT/llvm/lib/libxml2.so.2" "$ROOT/packaging/linux/libxml2-stub/libxml2.c"

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
echo "==> SDL3"
if [ -z "$SDL3_PREFIX" ]; then
    SDL3_PREFIX="$WORK/sdl3"
    "$ROOT/packaging/linux/build-sdl3.sh" "$SDL3_PREFIX"
fi
mkdir -p "$OUT/lib" "$S/usr/include"
cp -L "$SDL3_PREFIX/lib/libSDL3.so.0" "$OUT/lib/"
# (SDL3's headers need none of X11's or Wayland's: unlike SDL2's SDL_syswm.h, nothing of the window drivers is in them)
cp -r "$SDL3_PREFIX/include/SDL3" "$S/usr/include/"
# SDL3 as the game's CMakeLists.txt finds it (find_package(SDL3 CONFIG))
mkdir -p "$S/usr/lib/cmake/SDL3"
cat > "$S/usr/lib/cmake/SDL3/SDL3Config.cmake" <<'CFG'
# SDL3 in the release's tools (packaging/linux/toolchain.sh): its headers in the sysroot, and the library
# shipped beside the game (toolchain/lib), which the game finds through its run path ($ORIGIN)
get_filename_component(_sysroot "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT TARGET SDL3::SDL3)
    add_library(SDL3::SDL3 SHARED IMPORTED)
    set_target_properties(SDL3::SDL3 PROPERTIES
        IMPORTED_LOCATION "${_sysroot}/../lib/libSDL3.so.0"
        IMPORTED_SONAME "libSDL3.so.0"
        INTERFACE_INCLUDE_DIRECTORIES "${_sysroot}/usr/include")
endif()
set(SDL3_FOUND TRUE)
set(SDL3_VERSION 3.4.20)
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
# libstdc++ and libgcc inside the program: it needs only the C library, Vulkan and SDL3 from the system
# and SDL3 found beside it ($ORIGIN), never in the tools' folder
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -static-libstdc++ -static-libgcc -Wl,-rpath,\\$ORIGIN") # (\$: past the shell)
set(CMAKE_SKIP_BUILD_RPATH ON)
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -static-libstdc++ -static-libgcc")
# the synchronization2 layer (cmake/vk_sync2_layer.cmake) is a MODULE, which reads its own flags
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld -static-libstdc++ -static-libgcc")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_PREFIX_PATH "/usr/lib/cmake")
# no pkg-config: the build finds libdrm by its files (CMakeLists.txt)
set(PKG_CONFIG_EXECUTABLE "" CACHE FILEPATH "")
CFG
echo "==> the tools need only what every Linux has"
# A tool that needs a library the player may lack fails with "unable to execute command" (lld and
# libxml2.so.2 on Arch, 2026-11-23). Each one's libraries must be on this list or shipped beside it.
system_libs=" libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 ld-linux-x86-64.so.2 libz.so.1 libstdc++.so.6 libgcc_s.so.1 "
find "$OUT/llvm/bin" "$OUT/cmake/bin" "$OUT/bin" -type f | while read -r tool; do
    head -c 4 "$tool" | grep -q ELF || continue
    for lib in $(readelf -d "$tool" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        case "$system_libs" in *" $lib "*) continue ;; esac
        [ -f "$(dirname "$tool")/../lib/$lib" ] && continue
        echo "toolchain.sh: $tool needs $lib, which is neither shipped nor on every Linux" >&2
        exit 1
    done
done
echo "$ID" > "$OUT/ID"
echo "done: $OUT ($(du -sh "$OUT" | cut -f1))"

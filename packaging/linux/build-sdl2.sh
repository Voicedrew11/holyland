#!/usr/bin/env bash
# SDL2 for a Linux release (docs/port/LAUNCHER.md, "The tools"), built from its source on the machine making
# the release, and shipped with the game and the launcher. Every system library it can use (X11, Wayland,
# PulseAudio, PipeWire, ALSA, libdecor, KMS/DRM...) is loaded when it runs, not linked, so the one file runs
# on any distribution: build it where the oldest supported one is (CI: ubuntu-22.04, with the -dev packages
# .github/workflows/release.yml installs) so it needs no newer C library.
#
#   packaging/linux/build-sdl2.sh PREFIX     installs include/SDL2 and lib/libSDL2-2.0.so.0 into PREFIX
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFIX="${1:?usage: packaging/linux/build-sdl2.sh PREFIX}"
PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"
mkdir -p "$ROOT/out"
WORK="$(mktemp -d "$ROOT/out/sdl2-work.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

tar -xzf "$("$ROOT/packaging/fetch.sh" sdl2-source)" -C "$WORK"
cmake -G Ninja -S "$WORK/SDL2-2.30.9" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL2_DISABLE_SDL2MAIN=ON \
    -DSDL_DEPS_SHARED=ON -DSDL_RPATH=OFF
cmake --build "$WORK/build"
cmake --install "$WORK/build"
# what it needs from the system when it runs: only the C library, the rest it loads itself
echo "libSDL2 needs:"
objdump -p "$PREFIX/lib/libSDL2-2.0.so.0" | awk '/NEEDED/ { print "  " $2 }'
# a release without sound or a window would be silent or blank on players' machines, not fail: refuse it
loads="$(strings "$PREFIX/lib/libSDL2-2.0.so.0")"
for want in libX11 libwayland-client 'libpulse|libpipewire|libasound'; do
    grep -Eq "^($want)[^ ]*\.so" <<<"$loads" && continue
    if [ "${HOLYLAND_PARTIAL_SDL2:-0}" = 1 ]; then
        echo "build-sdl2.sh: WARNING: no $want (allowed by HOLYLAND_PARTIAL_SDL2=1: a test build only)" >&2
    else
        echo "build-sdl2.sh: SDL2 was built without $want: install its -dev package (see release.yml)" >&2
        exit 1
    fi
done
echo "its newest C library symbol: $(objdump -T "$PREFIX/lib/libSDL2-2.0.so.0" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"

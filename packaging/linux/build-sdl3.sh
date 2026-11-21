#!/usr/bin/env bash
# SDL3 for a Linux release (docs/port/LAUNCHER.md, "The tools"), built from its source on the machine making
# the release, and shipped with the game and the launcher. Every system library it can use (X11, Wayland,
# PulseAudio, PipeWire, ALSA, libdecor, KMS/DRM...) is loaded when it runs, not linked, so the one file runs
# on any distribution: build it where the oldest supported one is (CI: ubuntu-22.04, with the -dev packages
# .github/workflows/release.yml installs) so it needs no newer C library.
#
#   packaging/linux/build-sdl3.sh PREFIX     installs include/SDL3 and lib/libSDL3.so.0 into PREFIX
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFIX="${1:?usage: packaging/linux/build-sdl3.sh PREFIX}"
PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"
mkdir -p "$ROOT/out"
WORK="$(mktemp -d "$ROOT/out/sdl3-work.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

tar -xzf "$("$ROOT/packaging/fetch.sh" sdl3-source)" -C "$WORK"
cmake -G Ninja -S "$WORK/SDL3-3.4.20" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_DEPS_SHARED=ON -DSDL_RPATH=OFF
cmake --build "$WORK/build"
cmake --install "$WORK/build"
# what it needs from the system when it runs: only the C library, the rest it loads itself
echo "libSDL3 needs:"
objdump -p "$PREFIX/lib/libSDL3.so.0" | awk '/NEEDED/ { print "  " $2 }'
# a release without sound or a window would be silent or blank on players' machines, not fail: refuse it
loads="$(strings "$PREFIX/lib/libSDL3.so.0")"
for want in libX11 libwayland-client 'libpulse|libpipewire|libasound'; do
    grep -Eq "^($want)[^ ]*\.so" <<<"$loads" && continue
    if [ "${HOLYLAND_PARTIAL_SDL3:-0}" = 1 ]; then
        echo "build-sdl3.sh: WARNING: no $want (allowed by HOLYLAND_PARTIAL_SDL3=1: a test build only)" >&2
    else
        echo "build-sdl3.sh: SDL3 was built without $want: install its -dev package (see release.yml)" >&2
        exit 1
    fi
done
echo "its newest C library symbol: $(objdump -T "$PREFIX/lib/libSDL3.so.0" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"

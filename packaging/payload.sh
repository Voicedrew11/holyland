#!/usr/bin/env bash
# The release's source: what the launcher builds the game from on the player's computer
# (docs/port/LAUNCHER.md, "What a release holds"). Writes CONTENT/src:
#
#   - this checkout's tracked files that the game's build reads (not docs/, patches/, launcher/,
#     packaging/, .github/);
#   - decomp/src/game and src/nonmatching with patches/ applied (the build then needs no git:
#     KF4_DECOMP_PREPATCHED), and the decomp's libm and libc sources the build compiles;
#   - paraLLEl-GS with only the parts of Granite the build uses (volk, the Vulkan headers), with the
#     cmake/granite_*.cmake edits already made: the player's configure finds them and writes nothing,
#     so the source can be read-only (an AppImage, Program Files).
#
#   packaging/payload.sh CONTENT
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTENT="${1:?usage: packaging/payload.sh CONTENT}"
SRC="$CONTENT/src"
rm -rf "$SRC"
mkdir -p "$SRC"

# copy_tracked REPO DEST [path filters...]: the files git tracks in REPO (as they are in the working tree),
# under its paths matching the filters. One tar for all of them, not a cp per file: starting a process is slow
# on Windows (a cp per file took 3 minutes of the Windows release; the loop below uses only shell builtins).
copy_tracked() {
    local repo="$1" dest="$2"
    shift 2
    mkdir -p "$dest"
    git -C "$repo" ls-files -z --cached -- "$@" | while IFS= read -r -d '' f; do
        [ -f "$repo/$f" ] || continue # a submodule's entry, or a file deleted in the working tree
        printf '%s\0' "$f"
    done | tar -C "$repo" --null -T - -cf - | tar -C "$dest" -xpf -
}

echo "==> the port"
copy_tracked "$ROOT" "$SRC" . ':!docs' ':!patches' ':!launcher' ':!packaging' ':!.github' ':!decomp' \
    ':!third_party/parallel-gs' ':!*.md'
cp "$ROOT/LICENSE" "$SRC/" 2>/dev/null || true

echo "==> the decomp, patched"
DECOMP="$ROOT/decomp"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
copy_tracked "$DECOMP" "$STAGE" src/game src/nonmatching
git -C "$STAGE" init -q
# the order CMake applies them in (CMakeLists.txt: list(SORT) of the glob, plain byte order)
find "$ROOT/patches" -name '*.patch' -print0 | LC_ALL=C sort -z | while IFS= read -r -d '' patch; do
    git -C "$STAGE" apply --whitespace=nowarn "$patch"
done
mkdir -p "$SRC/decomp/src"
cp -r "$STAGE/src/game" "$STAGE/src/nonmatching" "$SRC/decomp/src/"
copy_tracked "$DECOMP" "$SRC/decomp" src/sce/libm src/sce/libc LICENSE

echo "==> paraLLEl-GS and the parts of Granite it needs"
PGS="$ROOT/third_party/parallel-gs"
GRANITE="$PGS/Granite"
[ -f "$GRANITE/third_party/volk/volk.h" ] || {
    echo "payload.sh: paraLLEl-GS's Granite is not checked out (CLAUDE.md: git submodule update --init, then" >&2
    echo "  Granite's volk and khronos/vulkan-headers)" >&2
    exit 1
}
# Granite's edits (cmake/granite_edits.cmake), as the game's configure makes them: none when already made
cmake -DGRANITE="$GRANITE" -P "$ROOT/cmake/apply_granite_edits.cmake"
copy_tracked "$PGS" "$SRC/third_party/parallel-gs" . ':!Granite'
copy_tracked "$GRANITE" "$SRC/third_party/parallel-gs/Granite" . ':!third_party'
# Granite's own files in third_party/ (its CMakeLists.txt skips what is not checked out); the submodules there
# are entries, not files, and copy_tracked passes over them
copy_tracked "$GRANITE" "$SRC/third_party/parallel-gs/Granite" ':(glob)third_party/*' third_party/renderdoc
copy_tracked "$GRANITE/third_party/volk" "$SRC/third_party/parallel-gs/Granite/third_party/volk" .
copy_tracked "$GRANITE/third_party/khronos/vulkan-headers" \
    "$SRC/third_party/parallel-gs/Granite/third_party/khronos/vulkan-headers" . ':!registry' # (its XML: not built)

# nothing in the release's source is to be written by the build
find "$SRC" -name .git -prune -exec rm -rf {} +
echo "done: $SRC ($(du -sh "$SRC" | cut -f1))"

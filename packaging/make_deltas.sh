#!/usr/bin/env bash
# A release's deltas from an older one (launcher/update_delta.h, docs/port/LAUNCHER.md "Deltas"): for each whole file
# this release (NEW_DIR) and the older one (OLD_DIR, its files as downloaded) both have, the small file that makes
# the new one from what the older one installed, written into NEW_DIR as "<new name>.from-<OLD_VERSION>.delta":
#   *-x86_64.AppImage  a file patch: zstd --patch-from the old AppImage
#   *-win-x64.zip      a tree delta: a zip of the files new or changed, and delta-files.txt (every file's SHA-256)
# The installer has none: its installations download the whole installer. A delta that saves little is dropped.
# packaging/check_deltas.sh checks them with the launcher's own code.
#
#   packaging/make_deltas.sh NEW_DIR OLD_DIR OLD_VERSION
set -euo pipefail
NEW_DIR="${1:?usage: make_deltas.sh NEW_DIR OLD_DIR OLD_VERSION}"
OLD_DIR="${2:?usage: make_deltas.sh NEW_DIR OLD_DIR OLD_VERSION}"
OLD_VERSION="${3:?usage: make_deltas.sh NEW_DIR OLD_DIR OLD_VERSION}"
LISTING=delta-files.txt # launcher/update_delta.h: kDeltaListing

# the one file in DIR whose name ends with SUFFIX, or nothing
one() {
    local found=("$1"/*"$2")
    [ "${#found[@]}" -eq 1 ] && [ -f "${found[0]}" ] && echo "${found[0]}"
}

# a delta kept only when it is under half the whole file
keep_if_small() {
    local delta="$1" whole="$2" delta_bytes whole_bytes
    delta_bytes=$(stat -c %s "$delta")
    whole_bytes=$(stat -c %s "$whole")
    if [ $((delta_bytes * 2)) -ge "$whole_bytes" ]; then
        echo "delta: $(basename "$delta") is $delta_bytes bytes of $whole_bytes, not worth it; dropped"
        rm -f "$delta"
    else
        echo "delta: $(basename "$delta"): $delta_bytes bytes (whole $whole_bytes)"
    fi
}

file_delta() {
    local new="$1" old="$2" out="$1.from-$OLD_VERSION.delta"
    # --long=30: the window holds the old file and the new one (the launcher's decoder takes up to 31)
    zstd -q -f -19 -T0 --long=30 --patch-from="$old" "$new" -o "$out"
    keep_if_small "$out" "$new"
}

# "<sha256>  <path>" for every file under DIR, sorted by path
tree_listing() {
    (cd "$1" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum | sed 's|  \./|  |')
}

tree_delta() {
    local new="$1" old="$2" out work
    out="$(cd "$(dirname "$new")" && pwd)/$(basename "$new").from-$OLD_VERSION.delta"
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' RETURN
    unzip -q "$old" -d "$work/old"
    unzip -q "$new" -d "$work/new"
    if [ -e "$work/new/$LISTING" ]; then
        echo "delta: $(basename "$new") holds a $LISTING of its own" >&2
        return 1
    fi
    tree_listing "$work/new" > "$work/new.txt"
    tree_listing "$work/old" > "$work/old.txt"
    mkdir "$work/stage"
    # the new or changed files: lines of new.txt that old.txt does not have
    LC_ALL=C sort "$work/old.txt" > "$work/old.sorted"
    LC_ALL=C sort "$work/new.txt" | LC_ALL=C comm -13 "$work/old.sorted" - | cut -c67- | while IFS= read -r path; do
        mkdir -p "$work/stage/$(dirname "$path")"
        cp -p "$work/new/$path" "$work/stage/$path"
    done
    cp "$work/new.txt" "$work/stage/$LISTING"
    rm -f "$out"
    (cd "$work/stage" && zip -q -X -r -9 "$out" .)
    keep_if_small "$out" "$new"
}

made=0
if new=$(one "$NEW_DIR" -x86_64.AppImage) && old=$(one "$OLD_DIR" -x86_64.AppImage); then
    file_delta "$new" "$old"
    made=1
fi
if new=$(one "$NEW_DIR" -win-x64.zip) && old=$(one "$OLD_DIR" -win-x64.zip); then
    tree_delta "$new" "$old"
    made=1
fi
[ "$made" = 1 ] || echo "delta: $OLD_DIR has no file this release has; no delta from $OLD_VERSION"

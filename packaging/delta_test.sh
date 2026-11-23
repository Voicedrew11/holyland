#!/usr/bin/env bash
# Deltas made and checked end to end on two made-up releases (ctest's "deltas"; CI): packaging/make_deltas.sh makes
# them, packaging/check_deltas.sh checks them with the launcher's own code, and a broken one must fail the check.
# Skipped (77) without zstd, zip or unzip.
#
#   packaging/delta_test.sh LAUNCHER_DELTA_CHECK
set -euo pipefail
CHECK="${1:?usage: delta_test.sh LAUNCHER_DELTA_CHECK}"
HERE="$(cd "$(dirname "$0")" && pwd)"
for tool in zstd zip unzip; do
    command -v "$tool" >/dev/null || { echo "delta_test: no $tool; skipped"; exit 77; }
done
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL $*" >&2; exit 1; }

# two releases: an "AppImage" of 4 MB with 64 KB changed in its middle and more at its end, and a "zip" with files
# kept, changed, added and removed (and a big kept one, as the tools are)
mkdir -p "$work/old" "$work/new" "$work/tree-old/content/src" "$work/tree-old/content/toolchain"
head -c 4000000 /dev/urandom > "$work/old/H-0.1.0-x86_64.AppImage"
{ head -c 2000000 "$work/old/H-0.1.0-x86_64.AppImage"; head -c 65536 /dev/urandom
  tail -c +2065537 "$work/old/H-0.1.0-x86_64.AppImage"; head -c 30000 /dev/urandom; } > "$work/new/H-0.2.0-x86_64.AppImage"
echo "launcher 0.1.0" > "$work/tree-old/holyland.exe"
echo "kept" > "$work/tree-old/content/src/kept.c"
echo "old text" > "$work/tree-old/content/src/changed.c"
echo "removed" > "$work/tree-old/content/src/removed.c"
echo "a name with spaces" > "$work/tree-old/content/src/with spaces.txt"
head -c 3000000 /dev/urandom > "$work/tree-old/content/toolchain/clang"
cp -r "$work/tree-old" "$work/tree-new"
echo "launcher 0.2.0" > "$work/tree-new/holyland.exe"
echo "new text" > "$work/tree-new/content/src/changed.c"
rm "$work/tree-new/content/src/removed.c"
mkdir -p "$work/tree-new/content/src/added"
echo "added" > "$work/tree-new/content/src/added/new.c"
(cd "$work/tree-old" && zip -q -r "$work/old/H-0.1.0-win-x64.zip" .)
(cd "$work/tree-new" && zip -q -r "$work/new/H-0.2.0-win-x64.zip" .)

"$HERE/make_deltas.sh" "$work/new" "$work/old" 0.1.0
for kind in x86_64.AppImage win-x64.zip; do
    [ -f "$work/new/H-0.2.0-$kind.from-0.1.0.delta" ] || fail "no delta for $kind"
done
unzip -l "$work/new/H-0.2.0-win-x64.zip.from-0.1.0.delta" | grep -q "toolchain/clang" && fail "the tree delta holds a kept file"
"$HERE/check_deltas.sh" "$work/new" "$work/old" 0.1.0 "$CHECK"

# a delta from another older release must not pass
head -c 4000000 /dev/urandom > "$work/other.AppImage"
mkdir "$work/other" && mv "$work/other.AppImage" "$work/other/H-0.0.9-x86_64.AppImage"
cp "$work/old/H-0.1.0-win-x64.zip" "$work/other/H-0.0.9-win-x64.zip"
"$HERE/check_deltas.sh" "$work/new" "$work/other" 0.1.0 "$CHECK" 2>/dev/null && fail "a patch against another AppImage passed"

# a tree delta with a file changed must not pass
(cd "$work" && mkdir broken && cd broken && unzip -q "$work/new/H-0.2.0-win-x64.zip.from-0.1.0.delta" &&
 echo "tampered" > content/src/changed.c && rm "$work/new/H-0.2.0-win-x64.zip.from-0.1.0.delta" &&
 zip -q -r "$work/new/H-0.2.0-win-x64.zip.from-0.1.0.delta" .)
rm "$work/new/H-0.2.0-x86_64.AppImage.from-0.1.0.delta"
"$HERE/check_deltas.sh" "$work/new" "$work/old" 0.1.0 "$CHECK" 2>/dev/null && fail "a changed tree delta passed"
echo "delta_test: good"

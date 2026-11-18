#!/usr/bin/env bash
# Download a pinned tool (packaging/toolchain.lock) into the cache, once, checked by its SHA-256; print the
# file's path.
#
#   packaging/fetch.sh NAME            the file's path (downloaded if not cached)
#   packaging/fetch.sh --print NAME    its SHA-256, for a new line in toolchain.lock
#
# The cache is $DARKER_REALITY_CACHE, else out/packaging-cache/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE="${DARKER_REALITY_CACHE:-$ROOT/out/packaging-cache}"
PRINT=0
if [ "${1:-}" = "--print" ]; then PRINT=1; shift; fi
NAME="${1:?usage: packaging/fetch.sh [--print] NAME}"

line="$(awk -v n="$NAME" '$1 == n { print $2, $3 }' "$ROOT/packaging/toolchain.lock")"
[ -n "$line" ] || { echo "fetch.sh: no $NAME in packaging/toolchain.lock" >&2; exit 1; }
want="${line%% *}"
url="${line#* }"
file="$CACHE/$(basename "$url")"

mkdir -p "$CACHE"
if [ ! -f "$file" ]; then
    echo "fetch.sh: downloading $url" >&2
    curl -fL --retry 3 -o "$file.part" "$url"
    mv "$file.part" "$file"
fi
have="$(sha256sum "$file" | cut -d' ' -f1)"
if [ "$PRINT" = 1 ]; then
    echo "$have"
    exit 0
fi
if [ "$want" != "$have" ]; then
    echo "fetch.sh: $NAME's SHA-256 is $have, packaging/toolchain.lock says $want" >&2
    exit 1
fi
echo "$file"

#!/usr/bin/env bash
# Every delta from OLD_VERSION in NEW_DIR (packaging/make_deltas.sh) checked with the launcher's own code
# (launcher_delta_check, built with the launcher's tests): each must make its whole file exactly from the older
# release's file in OLD_DIR. Fails on the first that does not.
#
#   packaging/check_deltas.sh NEW_DIR OLD_DIR OLD_VERSION LAUNCHER_DELTA_CHECK
set -euo pipefail
NEW_DIR="${1:?usage: check_deltas.sh NEW_DIR OLD_DIR OLD_VERSION CHECKER}"
OLD_DIR="${2:?}"
OLD_VERSION="${3:?}"
CHECK="${4:?}"

checked=0
for delta in "$NEW_DIR"/*".from-$OLD_VERSION.delta"; do
    [ -f "$delta" ] || continue
    new="${delta%.from-$OLD_VERSION.delta}"
    case "$new" in
    *-x86_64.AppImage)
        "$CHECK" file "$OLD_DIR"/*-x86_64.AppImage "$delta" "$new" ;;
    *-win-x64.zip)
        work="$(mktemp -d)"
        unzip -q "$OLD_DIR"/*-win-x64.zip -d "$work/old"
        unzip -q "$new" -d "$work/new"
        unzip -q "$delta" -d "$work/delta"
        status=0
        "$CHECK" tree "$work/old" "$work/delta" "$work/new" || status=$?
        rm -rf "$work"
        [ "$status" = 0 ] || exit "$status" ;;
    *)
        echo "check_deltas: $(basename "$delta") is no kind of delta the launcher takes" >&2
        exit 1 ;;
    esac
    checked=$((checked + 1))
done
echo "check_deltas: $checked delta(s) from $OLD_VERSION good"

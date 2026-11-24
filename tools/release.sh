#!/usr/bin/env bash
# Cut a release: tag HEAD vMAJOR.MINOR.PATCH (docs/port/LAUNCHER.md, "Making a release").
#
#   tools/release.sh 0.2.0
#
# It does NOT push, and builds nothing: tools/release_local.sh calls it, then builds the packages on this computer and
# opens a DRAFT release; publishing that draft is what players' launchers see.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
NEW="${1:-}"
[ -n "$NEW" ] || { echo "usage: tools/release.sh MAJOR.MINOR.PATCH"; exit 1; }
echo "$NEW" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo "not MAJOR.MINOR.PATCH: $NEW"; exit 1; }
OLD="$(bash tools/version.sh)"
# the update check only tells players about a higher number
[ "$(printf '%s\n%s\n' "$OLD" "$NEW" | sort -V | tail -1)" = "$NEW" ] && [ "$NEW" != "$OLD" ] ||
    { echo "v$NEW is not above the newest tag, v$OLD"; exit 1; }
# (untracked files are not in a release: packaging/payload.sh copies what git tracks)
[ -z "$(git status --porcelain --untracked-files=no --ignore-submodules=dirty)" ] || { echo "the working tree has changes"; exit 1; }
git rev-parse -q --verify "refs/tags/v$NEW" >/dev/null && { echo "tag v$NEW already exists"; exit 1; }
# shellcheck disable=SC1091
. packaging/package.env
git tag -a "v$NEW" -m "$DISPLAY_NAME v$NEW"
echo "v$OLD -> v$NEW, tagged at $(git rev-parse --short=9 HEAD)."
echo "Before publishing: check a launcher-built kf4 against a developer's build (tools/build_compare.py)."

#!/usr/bin/env bash
# Holyland's version, MAJOR.MINOR.PATCH, by the rule every release reads it by (docs/port/LAUNCHER.md,
# "Versions"; Verdite's scripts/version.sh):
#
#   1. $HOLYLAND_VERSION, which the release workflow sets from the tag it was pushed for;
#   2. else the newest vMAJOR.MINOR.PATCH tag reachable from HEAD;
#   3. else 0.0.0 (no tag yet).
#
# The tag is the version: there is no VERSION file to disagree with it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -n "${HOLYLAND_VERSION:-}" ]; then
    v="${HOLYLAND_VERSION#v}"
else
    v="$( { git -C "$ROOT" tag --merged HEAD --list 'v*' --sort=-v:refname 2>/dev/null || true; } \
        | sed -nE '/^v[0-9]+\.[0-9]+\.[0-9]+$/{s/^v//p;q;}')"
    v="${v:-0.0.0}"
fi
echo "$v" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo "not MAJOR.MINOR.PATCH: $v" >&2; exit 1; }
echo "$v"

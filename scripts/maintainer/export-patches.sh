#!/bin/sh
# export-patches.sh — MAINTAINERS ONLY. Regenerate patches/ from a PS2Recomp
# branch whose commits sit on top of the pinned PS2X_REF.
# Usage: export-patches.sh <ps2recomp-checkout> [branch]
# Each commit becomes patches/NNNN-<subject>.patch (git format-patch), which
# 03-build-runner.sh applies in order. Keep one fix per commit.
set -eu
. "$(dirname "$0")/../common.sh"
KFIV_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
REPO="${1:?usage: export-patches.sh <ps2recomp-checkout> [branch]}"
BRANCH="${2:-HEAD}"
git -C "$REPO" merge-base --is-ancestor "$PS2X_REF" "$BRANCH" ||
  { echo "error: $BRANCH is not based on PS2X_REF ($PS2X_REF)" >&2; exit 1; }
rm -f "$KFIV_ROOT"/patches/*.patch
git -C "$REPO" format-patch --quiet --no-signature --zero-commit \
  -o "$KFIV_ROOT/patches" "$PS2X_REF..$BRANCH"
ls "$KFIV_ROOT/patches"

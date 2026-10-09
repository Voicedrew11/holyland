#!/bin/sh
# dev-setup.sh — create the persistent PS2Recomp checkout you work in.
# Usage: dev-setup.sh [checkout-dir]   (default $PS2X_DEV, see common.sh)
# Clones PS2Recomp, makes branch `kfiv` at the pinned PS2X_REF and applies
# patches/*.patch on it with `git am`, one commit per patch. Edit and commit
# there (one fix per commit), build with dev-build.sh, and turn the branch
# back into patches/ with export-patches.sh. 03-build-runner.sh never touches
# this checkout.
set -eu
. "$(dirname "$0")/../common.sh"
REPO="${1:-$PS2X_DEV}"
[ ! -e "$REPO" ] || { echo "error: $REPO already exists" >&2; exit 1; }
git config user.email >/dev/null 2>&1 ||
  { echo "error: set git user.name / user.email first (git am needs them)" >&2; exit 1; }
git clone "$PS2X_URL" "$REPO"
git -C "$REPO" checkout -b kfiv "$PS2X_REF"
git -C "$REPO" submodule update --init --recursive
git -C "$REPO" am "$KFIV_ROOT"/patches/*.patch
# dev-build.sh overwrites this tracked file with the generated function table;
# keep that out of git status and out of commits.
git -C "$REPO" update-index --skip-worktree ps2xRuntime/src/runner/register_functions.cpp
echo "dev checkout ready: $REPO (branch kfiv, $(git -C "$REPO" rev-list --count "$PS2X_REF..kfiv") patches)"

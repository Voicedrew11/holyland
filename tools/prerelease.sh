#!/usr/bin/env bash
# Publish the current branch's HEAD as the preview (docs/port/LAUNCHER.md, "Previews"): the rolling `preview`
# prerelease on GitHub, replacing the last one, each package zipped with the PREVIEW_PASSWORD secret.
# .github/workflows/release.yml's preview job does the work; this asks for it by pushing the tag preview-<commit>
# (which the job deletes), so it works from any branch. HEAD must be pushed: the workflow builds what GitHub has.
#
#   tools/prerelease.sh            # start it and wait for it
#   tools/prerelease.sh --no-wait
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

WAIT=1
[ "${1:-}" = "--no-wait" ] && WAIT=0

branch="$(git rev-parse --abbrev-ref HEAD)"
[ "$branch" != HEAD ] || { echo "detached HEAD; check out the branch to publish"; exit 1; }
[ -z "$(git status --porcelain --ignore-submodules=dirty)" ] || { echo "the working tree has changes"; exit 1; }
git fetch -q origin "$branch"
[ "$(git rev-parse HEAD)" = "$(git rev-parse "origin/$branch")" ] ||
    { echo "HEAD is not origin/$branch; push it first (the workflow builds what GitHub has)"; exit 1; }
gh secret list | grep -q '^PREVIEW_PASSWORD\b' ||
    { echo "the PREVIEW_PASSWORD secret is not set: gh secret set PREVIEW_PASSWORD"; exit 1; }

sha="$(git rev-parse HEAD)"
tag="preview-${sha::9}"
git tag -f "$tag" "$sha" >/dev/null
git push -q -f origin "refs/tags/$tag"
git tag -d "$tag" >/dev/null
echo "==> preview ${sha::9} asked for ($tag)"
for _ in $(seq 30); do
    run="$(gh run list --workflow release.yml --event push --commit "$sha" --limit 10 \
        --json databaseId,headBranch --jq "[.[] | select(.headBranch == \"$tag\")][0].databaseId // empty")"
    [ -n "$run" ] && break
    sleep 3
done
[ -n "${run:-}" ] || { echo "could not find the run; see: gh run list --workflow release.yml"; exit 1; }
echo "    $(gh run view "$run" --json url --jq .url)"
[ "$WAIT" = 1 ] || exit 0
gh run watch "$run" --exit-status --interval 30 >/dev/null
echo "==> published: $(gh release view preview --json url --jq .url)"

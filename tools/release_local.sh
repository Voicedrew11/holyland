#!/usr/bin/env bash
# A release made on this computer, not on GitHub's runners (docs/port/LAUNCHER.md, "Making a release"): tags HEAD
# vX.Y.Z, builds the AppImage (in an Ubuntu 22.04 container, packaging/linux/Containerfile), the Windows zip
# (llvm-mingw cross-compiling the launcher) and installer (Inno Setup in Wine), builds the game from the AppImage as a
# player's launcher does, makes and checks the deltas from the last releases, signs SHA256SUMS, then pushes the tag
# and opens a DRAFT release with the files. Publishing the draft is what players' launchers see.
#
#   tools/release_local.sh X.Y.Z             everything, up to the draft
#   tools/release_local.sh X.Y.Z --no-upload stops before pushing anything (the files are in out/release-X.Y.Z/dist)
#
# Run again after a failure: a tag already at HEAD is kept, and the Linux tools are cached (out/toolchain-linux-*).
#
# Needs: podman; wine with Inno Setup at $ISCC_EXE (default ~/.wine/drive_c/InnoSetup/ISCC.exe); llvm-mingw's Linux
# build at $LLVM_MINGW (default the one in out/llvm-mingw-linux/); gh, logged in; the release key at
# $RELEASE_SIGNING_KEY_FILE (default ~/.config/holyland-release/release-signing-key.pem) or in $RELEASE_SIGNING_KEY.
#
# Not done here, unlike GitHub's Windows runner: a player's build from the Windows packages and the installer's
# update over the last release (packaging/windows/update_test.sh): both need Windows itself.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
NEW="${1:-}"
UPLOAD=1
[ "${2:-}" = "--no-upload" ] && UPLOAD=0
[ -n "$NEW" ] || { echo "usage: tools/release_local.sh MAJOR.MINOR.PATCH [--no-upload]"; exit 1; }
# shellcheck disable=SC1091
. packaging/package.env

say() { echo; echo "==> $*"; }
fail() { echo "release_local.sh: $*" >&2; exit 1; }

ISCC_EXE="${ISCC_EXE:-$HOME/.wine/drive_c/InnoSetup/ISCC.exe}"
LLVM_MINGW="${LLVM_MINGW:-$(ls -d "$ROOT"/out/llvm-mingw-linux/llvm-mingw-*-x86_64 2>/dev/null | tail -1)}"
KEY_FILE="${RELEASE_SIGNING_KEY_FILE:-$HOME/.config/holyland-release/release-signing-key.pem}"
REPO="$(gh repo view --json nameWithOwner -q .nameWithOwner)"

say "checks"
command -v podman >/dev/null || fail "no podman"
command -v wine >/dev/null && [ -f "$ISCC_EXE" ] || fail "no Inno Setup in Wine at $ISCC_EXE"
[ -n "$LLVM_MINGW" ] && [ -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" ] || fail "no llvm-mingw (set LLVM_MINGW)"
[ -n "${RELEASE_SIGNING_KEY:-}" ] || [ -f "$KEY_FILE" ] || fail "no release key at $KEY_FILE"
[ -z "$(git status --porcelain --untracked-files=no --ignore-submodules=dirty)" ] || fail "the working tree has changes"
git submodule status | grep -q '^[-+]' &&
    fail "the submodules are not at their commits: git submodule update --init --recursive"
if [ "$UPLOAD" = 1 ]; then
    branch="$(git rev-parse --abbrev-ref HEAD)"
    git fetch -q origin "$branch"
    [ "$(git rev-parse HEAD)" = "$(git rev-parse "origin/$branch")" ] ||
        fail "HEAD is not origin/$branch: push it first (the release's tag must be on GitHub's history)"
fi

if [ "$(git rev-parse -q --verify "refs/tags/v$NEW^{commit}" || true)" = "$(git rev-parse HEAD)" ]; then
    echo "v$NEW is already HEAD's tag: kept"
else
    tools/release.sh "$NEW"
fi
export HOLYLAND_VERSION="$NEW"
WORK="$ROOT/out/release-$NEW"
DIST="$WORK/dist"
rm -rf "$WORK"
mkdir -p "$DIST"

say "the AppImage, on Ubuntu 22.04"
podman build -q -t holyland-release-linux -f packaging/linux/Containerfile packaging/linux >/dev/null
tools_id="$(cat packaging/toolchain.lock packaging/fetch.sh packaging/linux/toolchain.sh packaging/linux/build-sdl3.sh \
    packaging/linux/libxml2-stub/* | sha256sum | cut -c1-16)"
# (label=disable: the tree is not relabelled for the container; keep-id: what it writes is the owner's)
podman run --rm --userns=keep-id --security-opt label=disable -v "$ROOT:$ROOT" -w "$ROOT" \
    -e HOME="$WORK/home" -e HOLYLAND_VERSION -e OUT="$DIST" -e APPIMAGE_EXTRACT_AND_RUN=1 \
    -e HOLYLAND_TOOLCHAIN_CACHE="$ROOT/out/toolchain-linux-$tools_id" \
    holyland-release-linux bash -c 'mkdir -p "$HOME" && git config --global --add safe.directory "*" &&
        packaging/linux/build-appimage.sh'
APPIMAGE="$DIST/$NAME-$NEW-x86_64.AppImage"
[ -f "$APPIMAGE" ] || fail "no AppImage made"
rm -rf "$DIST/$NAME.AppDir" "$DIST/$APP_ID-blank.png"

say "a player's build from it"
PLAYER="$WORK/player"
KF4_DATA_DIR="$PLAYER" "$APPIMAGE" --build-only || { tail -n 60 "$PLAYER/build.log"; fail "the player's build failed"; }
kf4="$(ls "$PLAYER"/builds/*/kf4)"
newest="$(objdump -T "$kf4" "$(dirname "$kf4")/libSDL3.so.0" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
echo "the game needs $newest"
[ "$(printf '%s\nGLIBC_2.35\n' "$newest" | sort -V | tail -1)" = GLIBC_2.35 ] || fail "the game needs more than GLIBC_2.35"

say "the Windows zip and installer"
# build-windows.sh names Windows paths with cygpath and runs iscc: Wine's own stand-ins for them
SHIMS="$WORK/shims"
mkdir -p "$SHIMS"
cat > "$SHIMS/cygpath" <<'SH'
#!/bin/sh
[ "$1" = -w ] || exit 1
WINEDEBUG=-all exec winepath -w "$2"
SH
cat > "$SHIMS/iscc" <<SH
#!/bin/sh
WINEDEBUG=-all exec wine "$ISCC_EXE" "\$@"
SH
chmod +x "$SHIMS/cygpath" "$SHIMS/iscc"
PATH="$SHIMS:$PATH" OUT="$DIST" LLVM_MINGW="$LLVM_MINGW" packaging/windows/build-windows.sh
[ -f "$DIST/$NAME-$NEW-win-x64.zip" ] || fail "no Windows zip made"
[ -f "$DIST/$NAME-$NEW-win-x64-setup.exe" ] || fail "no Windows installer made"
rm -rf "$DIST/win-x64"

say "deltas from the last releases"
cmake -S launcher -B out/delta-check -G Ninja -DLAUNCHER_WINDOW=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build out/delta-check >/dev/null
ctest --test-dir out/delta-check --output-on-failure >/dev/null || fail "the launcher's tests failed"
gh release list --repo "$REPO" --exclude-drafts --exclude-pre-releases --limit 20 --json tagName -q '.[].tagName' |
    awk -v this="v$NEW" '$0 != this && /^v[0-9]/ && n < 3 { print; n++ }' |
    while read -r tag; do
        old="$WORK/old/$tag"
        mkdir -p "$old"
        gh release download "$tag" --repo "$REPO" -D "$old" -p '*-x86_64.AppImage' -p '*-win-x64.zip' ||
            { echo "$tag: no files to make deltas from"; continue; }
        packaging/make_deltas.sh "$DIST" "$old" "${tag#v}"
        packaging/check_deltas.sh "$DIST" "$old" "${tag#v}" out/delta-check/launcher_delta_check
        rm -rf "$old"
    done

say "checksums, signed"
(cd "$DIST" && sha256sum -- * > SHA256SUMS)
cat "$DIST/SHA256SUMS"
RELEASE_SIGNING_KEY="${RELEASE_SIGNING_KEY:-$(cat "$KEY_FILE")}" tools/release_sign.sh sign "$DIST/SHA256SUMS"
ls -la "$DIST"

if [ "$UPLOAD" = 0 ]; then
    echo "Not uploaded (--no-upload). The files: $DIST"
    exit 0
fi
say "the draft release"
git push -q origin "refs/tags/v$NEW"
gh release create "v$NEW" "$DIST"/* --repo "$REPO" --verify-tag --draft --generate-notes --title "v$NEW"
echo "Draft: $(gh release view "v$NEW" --repo "$REPO" --json url -q .url)"
echo "Check it, then publish it on GitHub (players' launchers see it only then)."

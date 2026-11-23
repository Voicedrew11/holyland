#!/usr/bin/env bash
# A player's install and update on Windows, as players get them (docs/port/LAUNCHER.md, "Updates"). Run by the
# release workflow's windows-update job on Windows (Git Bash); any failure fails the release.
#
#   packaging/windows/update_test.sh DIST [OLD_SETUP]
#
# DIST: this release's files (the zip and the installer). OLD_SETUP: the last published release's installer.
#
# 1. The zip unpacked with Windows' own tar.exe (as the launcher's zip update does): the install checked.
# 2. OLD_SETUP installed silently into a folder whose name has spaces and brackets, then this release's installer
#    run over it as the launcher's update runs it (silent, no folder given: Windows finds the old one by the
#    AppId): the install checked, then a player's build from it.
#
# "Checked": the launcher and SDL3.dll beside content\, every file content\manifest.txt lists present with its
# checksum, and nothing in content\ the manifest does not list (an old release's file left behind).
# A Windows player's update from 0.1.3 to 0.1.4 left content\toolchain\cmake\bin\cmake.exe missing (2026-11-23).
set -euo pipefail

DIST="${1:?usage: packaging/windows/update_test.sh DIST [OLD_SETUP]}"
OLD_SETUP="${2:-}"
# (Inno Setup's switches start with /: Git Bash must not take them for paths)
export MSYS2_ARG_CONV_EXCL='*'
WORK="$(cygpath -u "${RUNNER_TEMP:-$TEMP}")/update test (brackets)"
rm -rf "$WORK"
mkdir -p "$WORK"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

# An install folder as the release made it: "" or why not
check_install() {
    local dir="$1" listed present
    [ -f "$dir/holyland.exe" ] || fail "$dir has no holyland.exe"
    [ -f "$dir/SDL3.dll" ] || fail "$dir has no SDL3.dll"
    [ -f "$dir/content/manifest.txt" ] || fail "$dir has no content/manifest.txt"
    (cd "$dir/content" && sha256sum --check --quiet --strict manifest.txt) ||
        fail "$dir/content: files missing or not the release's (above)"
    listed="$(sed -e 's/^[0-9a-f]\{64\} [ *]//' -e 's#^\./##' "$dir/content/manifest.txt" | sort)"
    present="$(cd "$dir/content" && find . -type f ! -path ./manifest.txt | sed 's#^\./##' | sort)"
    if [ "$listed" != "$present" ]; then
        diff <(echo "$listed") <(echo "$present") | head -40 >&2 || true
        fail "$dir/content: files the manifest does not list, or listed files missing (above)"
    fi
    echo "ok: $dir ($(echo "$listed" | wc -l) files)"
}

ZIP="$(ls "$DIST"/*-win-x64.zip)"
SETUP="$(ls "$DIST"/*-win-x64-setup.exe)"

echo "==> The zip, unpacked by Windows' tar.exe"
mkdir -p "$WORK/zip install"
"$(cygpath -u "$SYSTEMROOT")/System32/tar.exe" -xf "$(cygpath -w "$ZIP")" -C "$(cygpath -w "$WORK/zip install")"
check_install "$WORK/zip install"

if [ -z "$OLD_SETUP" ]; then
    echo "no earlier release's installer: the update over one not tested"
    exit 0
fi

echo "==> $(basename "$OLD_SETUP") installed, then this release's installer over it as the launcher runs it"
INSTALL="$WORK/Games (Test)/Darker Reality"
"$OLD_SETUP" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART "/DIR=$(cygpath -w "$INSTALL")" ||
    fail "the earlier release's installer failed ($?)"
[ -f "$INSTALL/holyland.exe" ] || fail "the earlier release did not install into $INSTALL"
OLD_MANIFEST="$(sha256sum "$INSTALL/content/manifest.txt")"
# (launcher/platform/windows.cpp's InstallerInstallation: these switches and /HOLYLANDRELAUNCH, which only starts the
# new launcher after)
"$SETUP" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART || fail "this release's installer failed ($?)"
cmp -s "$INSTALL/content/manifest.txt" "$WORK/zip install/content/manifest.txt" ||
    fail "the update did not go into $INSTALL (the old release's manifest is still there: $OLD_MANIFEST)"
check_install "$INSTALL"

echo "==> A player's build from the updated install"
KF4_DATA_DIR="$WORK/player" "$INSTALL/holyland.exe" --build-only ||
    { tail -n 60 "$WORK/player/build.log"; fail "the updated install did not build"; }
ls "$WORK"/player/builds/*/kf4.exe >/dev/null || fail "the build made no kf4.exe"
echo "ok: the update builds"

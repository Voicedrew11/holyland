#!/usr/bin/env bash
# CONTENT/manifest.txt: every file of the release's content/ with its SHA-256, one "sha256  path" a line,
# sorted. The launcher's build key hashes it (launcher/build_key.h), so any change to the shipped source or
# tools means a new build on the player's computer.
#
#   packaging/manifest.sh CONTENT
set -euo pipefail
CONTENT="${1:?usage: packaging/manifest.sh CONTENT}"
cd "$CONTENT"
rm -f manifest.txt
find . -type f ! -name manifest.txt -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sed 's|  \./|  |' > manifest.txt.new
mv manifest.txt.new manifest.txt
echo "manifest: $(wc -l < manifest.txt) files"

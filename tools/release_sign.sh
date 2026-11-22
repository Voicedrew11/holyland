#!/usr/bin/env bash
# A release's SHA256SUMS signed, and checked, with the project's Ed25519 release key (docs/port/LAUNCHER.md,
# "Updates"; as OpenCE's tools/release_sign.sh). The launcher puts an update in place only when SHA256SUMS.sig is
# good for deploy/release-key.pub (compiled into it: launcher/release_key.h) and the file's SHA-256 is in SHA256SUMS.
#
#   RELEASE_SIGNING_KEY="$(cat key.pem)" tools/release_sign.sh sign dist/SHA256SUMS   # writes dist/SHA256SUMS.sig
#   tools/release_sign.sh verify dist/SHA256SUMS                                       # 0 when it is good
#   tools/release_sign.sh public-key key.pem                                           # the 64 hex digits
#
# The key is a PEM Ed25519 private key (openssl genpkey -algorithm ed25519), kept in the RELEASE_SIGNING_KEY
# repository secret. The signature is the file's exact bytes signed, written as 128 hex digits and a line end.
# RELEASE_PUBLIC_KEY (64 hex digits) checks against another key than deploy/release-key.pub (a test's).
# Needs OpenSSL 3.
set -euo pipefail

usage() {
    echo "usage: RELEASE_SIGNING_KEY=<pem> $0 sign FILE | $0 verify FILE | $0 public-key PEM" >&2
    exit 2
}

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# raw bytes from hex on stdin (no xxd on every runner)
from_hex() {
    python3 -c 'import sys; sys.stdout.buffer.write(bytes.fromhex(sys.stdin.read().strip()))'
}
to_hex() {
    od -An -v -tx1 | tr -d ' \n'
}

[ $# -eq 2 ] || usage
case "$1" in
sign)
    [ -n "${RELEASE_SIGNING_KEY:-}" ] || { echo "$0: RELEASE_SIGNING_KEY is not set" >&2; exit 1; }
    printf '%s\n' "$RELEASE_SIGNING_KEY" > "$work/key.pem"
    openssl pkeyutl -sign -rawin -inkey "$work/key.pem" -in "$2" -out "$work/signature"
    { to_hex < "$work/signature"; echo; } > "$2.sig"
    "$0" verify "$2"
    ;;
verify)
    public="${RELEASE_PUBLIC_KEY:-$(tr -d ' \n' < "$root/deploy/release-key.pub")}"
    # an Ed25519 SubjectPublicKeyInfo: its fixed 12 bytes, then the key
    printf '302a300506032b6570032100%s' "$public" | from_hex > "$work/public.der"
    tr -d ' \n' < "$2.sig" | from_hex > "$work/signature"
    if openssl pkeyutl -verify -rawin -pubin -inkey "$work/public.der" -keyform DER -in "$2" \
        -sigfile "$work/signature" >/dev/null 2>&1; then
        echo "$2.sig: good"
    else
        echo "$2.sig: NOT good for this key" >&2
        exit 1
    fi
    ;;
public-key)
    openssl pkey -in "$2" -pubout -outform DER | tail -c 32 | to_hex
    echo
    ;;
*)
    usage
    ;;
esac

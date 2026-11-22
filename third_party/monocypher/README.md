# Monocypher 4.0.2

From https://monocypher.org/download/monocypher-4.0.2.tar.gz (SHA-256
`38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`), unchanged: `src/monocypher.{c,h}` and
`src/optional/monocypher-ed25519.{c,h}`. BSD-2-Clause or CC0 (`LICENCE.md`).

The launcher uses `crypto_ed25519_check` alone: a release's `SHA256SUMS` is checked against the project's
release key before an update is put in place (`launcher/self_update.cpp`, docs/port/LAUNCHER.md "Updates").

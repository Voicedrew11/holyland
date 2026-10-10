# Notes for AI coding agents

KFIV-PC statically recompiles King's Field: The Ancient City (PS2, USA
`SLUS-20318`) with [PS2Recomp](https://github.com/ran-j/PS2Recomp). This
repository holds tooling and per-game metadata; the user supplies the disc.
Native Windows is verified. The original Linux workflow remains available.

## Read first

- `README.md`: current status and repository scope.
- `docs/windows.md` and `docs/windows-validation.md`: Windows build steps,
  observed behaviour and verification limits.
- `docs/building.md`: Linux scripts `00`–`04`.
- `docs/contributing.md`: persistent development checkout and patch export.
- `docs/NOTES.md`: current problems and historical findings.
- `tests/README.md`: synthetic fixtures, actual/substituted boundaries and
  configurable source/build paths.

## Setting a user up

1. Use the user's own US disc image. Verify `SLUS_203.18` against the
   committed hash; never download game data.
2. Follow the guide for their platform and keep extracted disc files,
   generated sources, build trees and test runs outside this repository.
3. Apply generator patches and build the patched tools **before** generating
   game C++. Generation must report zero errors. A stale generated function
   table misses the residual-entry fix even when the runtime is rebuilt.
4. Apply the full runtime series, stage the complete generated output and
   build the runner. Do not imply a successful fixture build proves retail
   gameplay, hardware accuracy or a full playthrough.

## Where code lives

- Game-derived C++ belongs in `<game-dir>/output` and a private runtime
  checkout. Regenerate it rather than editing or committing it.
- Runtime and generator changes are developed as commits in a persistent
  PS2Recomp checkout and exported to `patches/`; never hand-edit patch files.
- `scripts/apply-patches.py` applies the pinned series without resetting
  local changes. Its verified phase records live in Git's private directory.
  A wrong HEAD or changed managed file fails safely. Use a fresh checkout
  when changing an already applied series.
- Linux `03-build-runner.sh` recreates `<game-dir>/_build`; do not develop
  there. Use the maintainer checkout described in `docs/contributing.md`.
- The function map and stub policy are in `kfiv/`; map regeneration needs
  Ghidra, as described in `docs/maintainers.md`.

## Rules

- No disc assets, ELF, generated retail C++, memory cards, frame dumps or
  retail traces in Git. Source-only synthetic test input is permitted.
- Follow PS2 hardware behaviour using PCSX2 as reference. Keep diagnostics
  off by default behind environment variables. Game-specific overrides,
  if needed, belong in metadata-keyed override modules.
- Use hidden, isolated test runs when possible. Windows `Test-KFIV.ps1`
  creates private memory cards and retains logs; its default run directory
  is under the user's temp directory.
- Verify performance changes bit-exact with GS replay or the VU1 verifier.
  Describe native Windows and Linux checks separately; do not report a
  shell syntax check as Linux runtime validation.
- Patches and tests are GPL-3.0. Other repository tooling is MIT; preserve
  third-party notices, including the RecompOne SPU and FFmpeg notices.

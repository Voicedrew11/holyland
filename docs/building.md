# Building and running (Linux)

You need a legally owned disc image of the US release (`SLUS-20318`) and the
tools below. You do **not** need Ghidra: the function map is committed in
`kfiv/`. (Regenerating it is a maintainer task, see
[`maintainers.md`](maintainers.md).)

## Prerequisites

- CMake 3.20+, a C++20 compiler (GCC 13+ / Clang 16+), Ninja or Make, git,
  Python 3 (stdlib only).
- Runner only: X11 dev headers (raylib/GLFW) and the FFmpeg SDK. Fedora:
  `sudo dnf install libXinerama-devel libXrandr-devel libXcursor-devel
  libXi-devel ffmpeg-devel`.
- Roughly 5 GB free for the unpacked disc (about 1 GB) plus ~200 MB of
  generated C++ and the runner build.

## Steps

All scripts take an optional game dir (default `~/.local/share/kfiv-pc`),
where unpacked disc files, generated code and the runner build live. Keep it
outside this repo.

```sh
# 0. build the PS2Recomp tools once (clones to ~/src/PS2Recomp at the pinned
#    commit; override with PS2X_REPO / PS2X_BUILD / PS2X_REF)
./scripts/00-build-tools.sh

# 1. unpack your disc (whole tree; verifies the boot ELF's sha256)
./scripts/01-extract.sh "disc/King's Field The Ancient City.iso"

# 2. recompile the boot ELF to C++ using kfiv/config.toml + the function map
./scripts/02-recompile.sh            # expect "errors: 0", ~28k generated files

# 3. build the runner (clones PS2Recomp, applies patches/, long link step)
./scripts/03-build-runner.sh

# 4. run
./scripts/04-run.sh
```

## What each step does

- **Extract**: unpacks the disc with a stdlib-only ISO9660 reader and checks
  `SLUS_203.18` against `kfiv/SLUS_203.18.sha256`. A different release fails
  here, because the function map only matches that exact build. `DUMMY*.OUT`
  padding files are skipped.
- **Recompile**: rewrites `input`/`output`/`ghidra_output` in
  `kfiv/config.toml` for your game dir and runs `ps2_recomp`. Output is about
  200 MB across ~28k files; the only warnings expected are `JR`/`JALR`
  fallback promotions (switch tables).
- **Build runner**: clones PS2Recomp into `<game-dir>/_build`, checks out the
  pinned commit, applies [`patches/`](../patches), and builds
  `ps2EntryRunner` with the generated code. `-msse4.1` is forced (upstream
  sets no x86 flags) and `TMPDIR` is redirected into the game dir because the
  link step needs more space than a typical tmpfs `/tmp`.
- **Run**: the runner takes the ELF as `argv[1]` and reads the unpacked disc
  files from its working directory, so the script runs from the game dir.
  Memory cards (`mc0/`, `mc1/`) are created there too.

## Controls (keyboard)

WASD/arrows = d-pad + stick, X/Space = cross, C/Esc = circle, Z = square,
V = triangle, Q/E = L1/R1, Shifts = L2/R2, Enter = Start, Tab = Select.
Gamepads work if connected.

## Troubleshooting

- `sha256sum -c` fails in step 1: wrong release or a bad dump. Only the USA
  release (`SLUS-20318`) is supported.
- Link fails with "No space left on device": point `TMPDIR` at a larger disk
  (the script already uses the game dir).
- Missing `X11/extensions/Xinerama.h` or similar: install the X11 dev
  packages listed above.

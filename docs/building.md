# Building and running (Linux)

For native Windows, use [`windows.md`](windows.md). A native Ubuntu/WSL2 runner
and focused runtime/VU tests now build successfully, and the CPU renderer has
completed gameplay. WSL's Dozen Vulkan driver failed before gameplay; native
Linux GPU performance remains unverified. See [current Linux results and the
comparison with owner PR #11](fixed-frame-performance.md#ubuntu-and-the-owners-merged-work).
The validation reused privately generated EE sources; it was not a fresh
end-to-end extraction/recompilation run of every script below.

You need a legally owned disc image of the US release (`SLUS-20318`) and the
tools below. You do **not** need Ghidra: the function map is committed in
`kfiv/`. (Regenerating it is a maintainer task, see
[`maintainers.md`](maintainers.md).)

## Prerequisites

- CMake 3.20+, a C++20 compiler (GCC 13+ / Clang 16+), Ninja or Make, git,
  rsync, Python 3 (stdlib only).
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

- **Build tools**: requires a checkout at the pinned commit and applies the
  generator portions of the patch series before compiling `ps2_recomp`.
  The helper verifies completed phases on reruns and refuses a different
  HEAD or changed managed files rather than resetting them. Use a separate
  pinned checkout if your existing tools checkout holds other work.
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

The runner and maintainer build scripts enable the optional bounded native
VU1 catalog from your own ELF using `kfiv/vu1-native.json`. Generated C++ and
provenance stay in the build tree, outside generated EE unity batches.
Set `KFIV_VU_NATIVE=0` to use the interpreter throughout. Switching this
setting reconfigures the incremental build and clears both native input
cache values. Python 3.8+ is required when native VU1 compilation is enabled.
The canonical VU sources use strict floating point flags; the native backend
has been compiled and differentially tested on both Windows and Ubuntu.
The available WSL CPU-renderer fallback did not reach the fixed-frame target;
that result does not establish performance on a native Linux Vulkan driver.

The upstream census (`PS2X_VU1_CENSUS`) and static lift (`PS2X_VU1_LIFT`)
remain optional. The static lift is experimental and disabled by default;
its timing, callback and admission behavior is outside the bounded native
catalog's exact verification. Keep `PS2X_VU1_LIFT=0` when using or validating
the accepted native catalog. Maintainer builds still synchronize optional
`<game-dir>/vu1lift/*.cpp` sources for that separate experiment.

## Controls

The new series uses the Verdite bindings, verified on native Windows:
W/S move forward/back, A/D strafe, arrows move/turn and select menu entries,
Space attacks, F uses/confirms (hold to run), Q casts magic, Tab/Escape opens
the menu or goes back, Enter pauses, and Right Shift sends Select.

Click in gameplay to engage mouse look; that first click is consumed.
Left/right/middle mouse attack/cast/use. In menus, left mouse confirms the
current selection, right mouse goes back, and the wheel selects entries.
Escape/menu, pause and focus loss release capture; the small Verdite glyph
shows its state. Menu/pause return restores capture if it was engaged.

## Troubleshooting

- `sha256sum -c` fails in step 1: wrong release or a bad dump. Only the USA
  release (`SLUS-20318`) is supported.
- Link fails with "No space left on device": point `TMPDIR` at a larger disk
  (the script already uses the game dir).
- Missing `X11/extensions/Xinerama.h` or similar: install the X11 dev
  packages listed above.
- Low frame rate in 3D areas: expected for now (the VU1 interpreter is the
  bottleneck; see `docs/NOTES.md`). Use the Release runner `04-run.sh`
  picks, not a debug build. Graphics are rasterized on worker threads:
  `PS2X_GS_THREADS=<n>` overrides their number (default: half the CPU
  threads, at most 8).

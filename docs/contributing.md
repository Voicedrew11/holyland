# Contributing: changing the runtime

Most work on this port is in the PS2Recomp **runtime** (GS, VIF/VU1, IOP,
pad, audio...), not in this repo's scripts. The runtime changes live here as
`patches/*.patch` on top of the PS2Recomp commit pinned in
`scripts/common.sh` (`PS2X_REF`). This page is the loop for making one.

First get the game building and running once with
[`building.md`](building.md) (steps 00–04). You need its game dir
(`~/.local/share/kfiv-pc` by default) with the unpacked disc and `output/`.

## 1. Make a dev checkout (once)

```sh
./scripts/maintainer/dev-setup.sh        # -> ~/src/PS2Recomp-kfiv (PS2X_DEV)
```

This clones PS2Recomp, creates branch `kfiv` at `PS2X_REF` and applies
`patches/` with `git am`, so each patch is one commit. **Work here, not in
`<game-dir>/_build/repo`**: `03-build-runner.sh` deletes `_build` (it
refuses while `_build/repo` holds commits that exist nowhere else, but
uncommitted edits there are lost).

## 2. Build and test

The commands below are the existing Linux dev loop. Native Windows has a
separate [build guide](windows.md), isolated `scripts/windows/Test-KFIV.ps1`
and incremental `Rebuild-KFIV.ps1`. The [source-only fixtures](../tests/README.md)
need no disc assets; their documented substituted boundaries are deliberate.
Report Windows and Linux validation separately.

When a patch changes the generator, rerun step 00 to build the patched
tools, then regenerate the complete output in step 02 before staging it
into the runtime checkout. Merely relinking a runner with an old
`register_functions.cpp` cannot install the missing continuation entries.

```sh
./scripts/maintainer/dev-build.sh                       # incremental, -O2, no LTO
./scripts/maintainer/dev-run.sh runs/smoke 900          # headless, exits at tick 900
PS2X_RUNNER=~/.local/share/kfiv-pc/dev-build/ps2xRuntime/ps2EntryRunner \
  ./scripts/04-run.sh                                   # play it in a window
```

- The first `dev-build.sh` compiles all ~28k generated files (minutes).
  After that, changing a runtime `.cpp` recompiles that file and relinks
  in seconds. Changing a header that the game code includes
  (`ps2xRuntime/include/**`) recompiles everything; put declarations only
  the runtime needs in a header the generated code doesn't include.
  Re-running `02-recompile.sh` costs nothing extra unless the generated
  code actually changed (files are copied by content, not timestamp).
- `dev-build.sh` uses Ninja and mold when installed (`ninja-build`,
  `mold`). With `KFIV_CCACHE=1` it compiles through `ccache`: a fresh
  build tree, a reverted header edit or a switch back to an earlier branch
  then takes ~1 min instead of ~5. The precompiled header is off in that
  mode (GCC's isn't byte-identical between rebuilds, so it would defeat
  the cache), so a full rebuild that misses the cache, such as a real
  header change, takes ~2 min longer. Use it when you switch branches or
  rebuild trees often, not while editing headers. Switching the mode
  reconfigures and rebuilds once.
- Don't use `03-build-runner.sh` while iterating: it deletes `_build`,
  re-clones, and builds `-O3` with LTO, where almost all the time is one
  ~6 min link. Run it once at the end (step 3).
- `dev-run.sh <out-dir> <exit-tick> [VAR=value ...]` runs with a hidden
  window in a private copy of the game dir (your memory cards are copied,
  not used) and writes `log.txt` and `final.png` into `<out-dir>`. 60 ticks
  = 1 s of game time. Useful runs:
  - title screen: `dev-run.sh runs/title 900`
  - into the first 3D area:
    `dev-run.sh runs/game 3000 PS2X_DUMP_EVERY=150 PS2X_STATS=1
    PS2X_INPUT="1100:START:10,1500:CROSS:10,1900:CROSS:10"`
    (START opens the title menu, CROSS picks New Game, CROSS confirms
    Brightness; in-game by ~tick 2400, 3D area after a fade at ~2900).
- Compare against a run of the unchanged series (build `kfiv` before your
  commits) with the same ticks, input and machine. The GS renders on worker
  threads, so frames at a given tick differ between runs unless both use
  `PS2X_GS_LOCKSTEP=1`.

### Dev harness variables (patches 0001, 0003, 0008, 0011, 0013, 0014)

All are off by default. `dev-run.sh` sets the first four for you.

| Variable | Effect |
|---|---|
| `PS2X_HIDDEN=1` | create the window hidden |
| `PS2X_DUMP_DIR=<dir>` | where PNGs go (default: cwd) |
| `PS2X_EXIT_TICK=<n>` | write `final.png` and exit at vsync tick n |
| `PS2X_CTRL=<file>` | command file polled ~10x/s, consumed on read: `press BTN[+BTN] <ticks>`, `shot <path.png>`, `tick <path.txt>`, `threads <path.txt>`, `quit` |
| `PS2X_DUMP_EVERY=<n>` | dump the presented frame every n ticks (`frame_NNNNNN.png`) |
| `PS2X_INPUT="t:BTN[+BTN]:dur,..."` | scripted pad presses at tick t for dur ticks. Buttons: `UP DOWN LEFT RIGHT START SELECT CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3` |
| `PS2X_STATS=1` | print VSync ticks/s and display flips/s (game fps; full speed is 30) |
| `PS2X_THREADS_AT_EXIT=1` | dump EE threads, semaphores and event flags at exit (what a stuck game waits on) |
| `PS2X_GS_TRACE=<file>` (+ `PS2X_GS_TRACE_FROM`/`_TO`) | GS register/transfer/primitive trace to a file, optionally limited to a range |
| `PS2X_VRAM_DUMP=<file>` | raw 4 MB GS memory at exit |
| `PS2X_NO_WEAVE=1` | present the raw interlaced field instead of the woven frame |
| `PS2X_GS_THREADS=<n>` | GS rasterizer worker threads (default min(8, cores/2)) |
| `PS2X_GS_LOCKSTEP=1` | wait for the GS workers after every batch, so guest timing matches synchronous rendering (needed to compare frame dumps between runs) |
| `PS2X_GS_RECORD=<file>` | record every GS backend call for `scripts/maintainer/gsreplay` (big: ~2.5 GB to tick 2300) |

Regression tools for the renderer (`gsreplay`) and VU1
(`scripts/maintainer/dev/vu1-verify.patch`) are described under "Dev loop"
in [`NOTES.md`](NOTES.md).

## 3. Commit and export

Commit in the dev checkout, **one fix per commit** with a message that says
why (the commit becomes the patch). Then:

```sh
./scripts/maintainer/export-patches.sh ~/src/PS2Recomp-kfiv kfiv
```

This rewrites `patches/` from `PS2X_REF..kfiv`. Never edit a `.patch` by
hand. To fix an existing patch, amend its commit (`git rebase` on `kfiv`),
then export again. Check that `03-build-runner.sh` still builds from a clean
`_build`, add a line for a new patch to [`patches/README.md`](../patches/README.md),
and commit `patches/` in this repo.

## Rules

- No game data in git: no disc image, ELF, `output/`, frame dumps of game
  content, or traces. `.gitignore` covers the usual names.
- Fixes must follow the PS2 hardware (PCSX2 is the reference), not KFIV
  special cases. Game-specific behaviour, if ever needed, goes in a
  `PS2_REGISTER_GAME_OVERRIDE` module keyed by ELF metadata.
- Debug output you add must be off by default, behind an env variable.
- Performance changes must be bit-exact: verify with `gsreplay` or the VU1
  verifier, not by eye.
- Record what you learn (including dead ends) in [`NOTES.md`](NOTES.md).
- Upstream-worthy fixes should also go to
  [PS2Recomp](https://github.com/ran-j/PS2Recomp); a patch is dropped here
  once it is merged and `PS2X_REF` moves past it.

## Updating `PS2X_REF`

Rebase `kfiv` onto the new upstream commit in the dev checkout, resolve
conflicts, set `PS2X_REF` in `scripts/common.sh`, export, and rebuild from
scratch with `03-build-runner.sh` and `02-recompile.sh` (the recompiler may
have changed too).

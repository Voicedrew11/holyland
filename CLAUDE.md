# holyland

The PC port of King's Field IV, built from the kfiv-decomp source. Start with
[docs/port/README.md](docs/port/README.md). [docs/port/RULES.md](docs/port/RULES.md) is for editing the game code
(`work/src/game`) only; renderer work starts below instead.

## The decomp is read-only

- Never edit `decomp/` (the kfiv-decomp submodule) or anything under `out/<preset>/decomp_src/` (the
  patched copy that is compiled; compiler errors point into it).
- Change game code only in `work/`: `python3 tools/patches.py apply`, edit `work/src/game`, then
  `python3 tools/patches.py save` and commit `patches/`. The same path below `work/` as below
  `decomp_src/` is the file to edit.
- Never hand-edit the files in `patches/`; `patches.py save` writes them.
- Fixes to the reconstruction itself (names, types, structs) go to kfiv-decomp, not here.
- `game/` and `port/` are ordinary source, edited in place.

## Before changing anything that draws

- Read [docs/port/RENDERER.md](docs/port/RENDERER.md) (where things are) and, in
  [docs/port/PC_RENDERER.md](docs/port/PC_RENDERER.md), only "Where it stands" at the top: what is built, what limits
  the pictures, what is next and the owner's decisions. The rest of that file is history, read when a section is named.
- **Every check in RENDERER.md's "The checks before a renderer change goes in" runs before the commit**: the build,
  ctest (`gsn_draw` among them), `vu1_test`, boot and frame tests, the change's switch byte for byte, the map tour, ASan
  + UBSan, and speed if it could cost. A change confined to `port/gs/pc/` skips the map tour and native/fast byte for
  byte (RENDERER.md says what stands in). One that cannot run is named in the commit with why, and the owner is asked
  first.
- Measuring on this machine (spots, picture comparisons, pace_test, sampling, sanitizers, disk): RENDERER.md,
  "Measuring on Linux". Put it, or point to it, in every subagent's brief: subagents see no memory.
- A new check that guards correctness (which draws can be taken, which results reused) is exact: it compares what is
  read. Never removed, loosened or replaced by a count to win speed (the owner, 2026-10-29).
- Measure before estimating: give a number for a gain only from a measure (a profile's share, a count of what a change
  would take), and say what it rests on.

## Tests never touch the player's data

Every run of the game in a test sets `KF4_DATA_DIR` to a temporary folder.

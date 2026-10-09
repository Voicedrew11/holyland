# Notes for AI coding agents

KFIV-PC is a native PC port of King's Field: The Ancient City (PS2, USA
`SLUS-20318`) built by static recompilation with
[PS2Recomp](https://github.com/ran-j/PS2Recomp). This repo holds only
tooling and per-game data; the user supplies the disc. Linux only.

## Read first

- `README.md`: what this is, current status.
- `docs/building.md`: build and run (scripts `00`–`04`).
- `docs/contributing.md`: the loop for changing the runtime (dev checkout,
  incremental build, headless test runs, exporting patches).
- `docs/NOTES.md`: current state, open problems, findings, dead ends.
  Read "Current state" before proposing work; update it when you learn
  something.

## Setting a user up

1. Confirm they have their own US disc image (`SLUS-20318`). Other
   releases fail the hash check in step 1 and nothing here works without it.
   Never download or suggest sources for game data.
2. Install the prerequisites in `docs/building.md`. The package names
   there are Fedora's; translate them for other distros.
3. Run `00`–`04` in order. `02` must print `errors: 0`. `03` takes a long
   time (link step); run it in the background.
4. To change code: `docs/contributing.md`.

## Where code lives

- The game's code is generated C++ in `<game-dir>/output` (default game dir
  `~/.local/share/kfiv-pc`). Never edit or commit it; regenerate with
  `02-recompile.sh`.
- Runtime fixes are commits in the dev checkout (`~/src/PS2Recomp-kfiv`,
  made by `scripts/maintainer/dev-setup.sh`), exported to `patches/` with
  `scripts/maintainer/export-patches.sh`. Never edit `patches/*.patch` by
  hand.
- Never work in `<game-dir>/_build`: `03-build-runner.sh` deletes it.
- Per-game recompiler data: `kfiv/` (config, function map). Regenerating the
  function map needs Ghidra: `docs/maintainers.md`.

## Rules

- No game data in git (disc image, ELF, `output/`, traces, frame dumps).
- Fixes follow PS2 hardware behaviour (PCSX2 as reference), not
  KFIV-specific hacks. Debug output is off by default, behind an env var.
- You usually can't watch the game window: test with
  `scripts/maintainer/dev-run.sh` (headless, scripted input, PNG frame
  dumps, exits by itself) and read the PNGs or the log.
- Performance changes must be verified bit-exact (`gsreplay`, VU1
  verifier; see `docs/NOTES.md`, "Dev loop").
- Patches are GPL-3.0 (derived from PS2Recomp); the rest is MIT.

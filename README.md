# KFIV-PC — King's Field: The Ancient City (PC port via static recompilation)

Native PC port of **King's Field: The Ancient City** (FromSoftware, 2002,
US release `SLUS-20318`) built with
[PS2Recomp](https://github.com/ran-j/PS2Recomp), a PlayStation 2 static
recompiler that translates the game's MIPS R5900 ELF into C++ and runs it
against a portable runtime: no emulation loop, a real native binary.

> **You must supply your own disc image.** This repo contains no game data.
> You recompile from your legally owned copy of the US release; the build
> checks the boot ELF's hash and refuses anything else.

## Status: early, boots but not playable

- Recompiles cleanly: 28,008 functions, 27,719 recompiled, 289 SDK stubs,
  **0 errors**.
- Boots through EE/IOP init, loads the real IOP modules, and runs the game's
  main loop. Keyboard and gamepad input reach the game.
- A GIF `NLOOP=0` bug that corrupted the display registers (no text or
  textures) is fixed in [`patches/`](patches); rendering still needs
  verifying from a clean boot.
- No audio yet (not implemented in the PS2Recomp runtime). Not a complete
  game yet.

Details and open questions: [`docs/NOTES.md`](docs/NOTES.md).

## How it works (N64Recomp model)

This repo ships the tooling and the per-game data; you recompile from your own
disc. The generated C++ is the game's own code translated, so it is never
distributed. The per-game data in [`kfiv/`](kfiv) is small:

- `config.toml`: recompiler config with the SDK stub list;
- `SLUS_203.18.functions.csv`: function boundaries (from a Ghidra analysis), so
  **you don't need Ghidra**;
- `SLUS_203.18.sha256`: the exact boot ELF this data matches.

## Quick start (Linux)

1. Put your disc image in [`disc/`](disc) (it is git-ignored), or anywhere.
2. Install the prerequisites in [`docs/building.md`](docs/building.md).
3. Run:

```sh
./scripts/00-build-tools.sh                          # once: build PS2Recomp tools
./scripts/01-extract.sh "disc/King's Field The Ancient City.iso"
./scripts/02-recompile.sh                            # expect errors: 0
./scripts/03-build-runner.sh                         # long link step
./scripts/04-run.sh
```

Everything generated (unpacked disc, C++ output, runner build) goes to
`~/.local/share/kfiv-pc` by default, outside this repo. Pass a game dir as the
last argument to any script to change it. Full walkthrough, controls and
troubleshooting: [`docs/building.md`](docs/building.md).

## Layout

```
KFIV-PC/
├── kfiv/             per-game data: config, function map, ELF hash
├── patches/          patches applied to PS2Recomp at runner build time
├── scripts/          00-build-tools .. 04-run (end users), maintainer/ (Ghidra-side)
├── disc/             put your disc image here (contents git-ignored)
├── docs/
│   ├── building.md     step-by-step build and run guide
│   ├── maintainers.md  regenerating the function map (Ghidra), patch workflow
│   └── NOTES.md        work log: findings, ruled-out avenues
├── .recomp.json      project descriptor (PS2Recomp game-project format)
└── LICENSE           MIT (patches/ are GPL-3.0, derived from PS2Recomp)
```

## Next steps

1. Confirm textures and text render after the GIF fix; triage what the first
   in-game screens expose.
2. Per-game overrides via PS2Recomp's `PS2_REGISTER_GAME_OVERRIDE`, keyed by
   ELF metadata, never global hacks.
3. Upstream the runtime fixes in `patches/` to PS2Recomp.
4. Packaging, once it is playable.

## Acknowledgments

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j (GPL-3.0),
  inspired by N64Recomp; ELF parsing via ELFIO, TOML via toml11, formatting
  via fmt; runtime reference PCSX2.
- Methodology informed by
  [ps2recomp-workbench](https://github.com/phmdacosta/ps2recomp-workbench)
  (Windows-focused; this repo adapts it to Linux).

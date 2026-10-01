# KFIV-PC — King's Field: The Ancient City (PC port via static recompilation)

Native PC port of **King's Field: The Ancient City** (FromSoftware, 2002,
US release `SLUS-20318`) built with
[PS2Recomp](https://github.com/ran-j/PS2Recomp), a PlayStation 2 static
recompiler that translates the game's MIPS R5900 ELF into C++ and runs it
against a portable runtime — no emulation loop, a real native binary.

> **You must supply your own disc image.** Nothing in this repo contains game
> data, and the build requires your legally owned copy of the game. The ISO
> name in this folder is an example; any dump of the same release works.

## Status: exploring (day 1)

First analyzer-only pass on `SLUS_203.18` recompiles **cleanly**:

| Metric | Result |
|---|---|
| Functions processed | 3208 |
| Recompiled | 3019 |
| Stubs (SDK bindings) | 189 |
| Skipped | 0 |
| Decode failures | 0 |
| Errors | **0** |
| Output | 3218 `.cpp` files, ~84 MB |

No runaway function boundaries (the classic failure mode on other titles).
Remaining warnings are `JR`/`JALR` fallback promotions for switch tables,
which is normal. See [`docs/NOTES.md`](docs/NOTES.md) for the work log.

## How it works (N64Recomp model)

Ship the tooling plus the per-game configuration; the user recompiles from
their own disc. The generated C++ is the game's own code translated, and the
disc image is needed at run time anyway (the game issues raw sector reads).
The per-game configuration (`kfiv/config.toml` + function map, ~60 KB) **is
the deliverable** — everything else is derived.

## Layout

```
KFIV-PC/
├── README.md               this file
├── .recomp.json            third-party project descriptor (recomp.fyi schema)
├── kfiv/
│   └── config.toml         recompiler config: analyzer SDK stubs + MMIO/perf data
├── scripts/
│   ├── extract_elf.py      ISO -> boot ELF (stdlib only, no 7z needed)
│   ├── 01-extract.sh       pull SLUS_203.18 out of your ISO
│   ├── 02-analyze.sh       run ps2_analyzer (SDK stub names)
│   ├── 03-recompile.sh     run ps2_recomp (ELF -> C++)
│   ├── 04-build-runner.sh  build ps2EntryRunner with generated sources
│   └── 05-run.sh           run the port (needs the disc image)
└── docs/
    ├── NOTES.md            work log: blocker, findings, ruled-out avenues
    └── workflow.md         end-to-end Linux pipeline
```

Game data (`ISO`, extracted `ELF`, generated `output/`, `_build/`) lives
**outside git** — see [`.gitignore`](.gitignore). Recommended local layout:

```
~/.local/share/kfiv-pc/     # or anywhere outside this repo
├── SLUS_203.18             # extracted boot ELF
├── output/                 # generated C++ (disposable, regenerated)
└── _build/                 # PS2Recomp clone + runner build (disposable)
```

## Quick start (Linux)

```sh
# 0. one-off: clone + build the PS2Recomp tools (analyzer + recompiler)
git clone --recurse-submodules https://github.com/ran-j/PS2Recomp.git /path/to/PS2Recomp
cmake -S /path/to/PS2Recomp -B /path/to/PS2Recomp-build \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_RUNTIME=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/PS2Recomp-build --config Release -j"$(nproc)"

# 1. extract the boot ELF from your disc
./scripts/01-extract.sh "/path/to/King's Field The Ancient City.iso" ~/.local/share/kfiv-pc

# 2-3. analyze + recompile
./scripts/02-analyze.sh ~/.local/share/kfiv-pc   # refreshes kfiv/config.toml stubs
./scripts/03-recompile.sh ~/.local/share/kfiv-pc # ELF -> output/*.cpp, expect errors: 0

# 4-5. build the runner and play (see docs/workflow.md for GUI deps)
./scripts/04-build-runner.sh ~/.local/share/kfiv-pc
./scripts/05-run.sh ~/.local/share/kfiv-pc "/path/to/King's Field The Ancient City.iso"
```

Ghidra boundary refinement (recommended before serious debugging) is
documented in [`docs/workflow.md`](docs/workflow.md) step 4.

## Next steps

1. **Ghidra import/export** for exact function boundaries (analyzer heuristics
   alone miss `J`-only targets); merge analyzer stubs + Ghidra CSV.
2. **Runner build + boot test**; triage `guest-branch:missing-target` /
   syscall TODOs per `docs/workflow.md`.
3. **Game overrides** (`ps2xRuntime` `PS2_REGISTER_GAME_OVERRIDE`) for
   per-build routing, keyed by ELF metadata — never global hacks.
4. Packaging deferred until it boots (measure `/O2` compile time on the
   ~84 MB of generated C++).

## Acknowledgments

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j (GPL-3.0),
  inspired by N64Recomp; ELF parsing via ELFIO, TOML via toml11, formatting
  via fmt; runtime reference PCSX2.
- End-to-end methodology informed by
  [ps2recomp-workbench](https://github.com/phmdacosta/ps2recomp-workbench)
  (Windows-focused; this repo adapts it to Linux).

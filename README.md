# KFIV-PC — King's Field: The Ancient City

A native PC port of **King's Field: The Ancient City** (FromSoftware, USA
`SLUS-20318`) built with [PS2Recomp](https://github.com/ran-j/PS2Recomp).
The recompiler translates the game's MIPS R5900 executable into C++ compiled
for the host CPU. The supporting runtime interprets IOP code and executes VU1
through an interpreter or an optional bounded native catalog built from your ELF.
An optional Vulkan GS backend renders on the GPU; the software GS remains
available as a reference and compatibility fallback.

**Supply your own US disc image.** This repository contains tooling,
function boundaries and patches, with no game executable, generated game
code or disc assets. The build checks the boot ELF's hash.

## Current status

- Native Windows x64 boots into the first gameplay area. Title screens,
  menus, movement, attack input, inventory and pause/resume have been checked.
- Progression through the cave door into the next map is verified. Correct
  EE square-root operands prevent the floor fall; complete DMA chains retain
  the FINISH packet needed to render the next area. Movement, camera and
  inventory work past the original failure point.
- The first NPC's seated pose is restored. Shared EE CVT.W.S truncation
  corrects angle range reduction used by the original skeletal routines.
- Gameplay's vertical strip corruption is fixed: walls and sky now align
  across the original framebuffer feedback passes, and scanout preserves
  all 448 gameplay rows.
- Vulkan GS rendering was verified on native Windows with an RTX 4090.
  It handles rasterization, textures, transfers and scanout on the GPU;
  the existing window still receives a read-back RGBA image. See
  [Vulkan rendering](docs/vulkan.md) for setup, timing and limitations.
- Music, sound effects and opening-movie audio play through the game's
  sound driver and SPU2 implementation. The opening movie now displays its
  pictures and ends naturally in gameplay. Enter optionally skips the
  opening through the game's original Start input.
- Final generation processed 28,429 functions: 28,161 recompiled, 268 SDK
  stubs, 1,352 JR/JALR fallback warnings and **zero errors**.
- Gameplay timing is independent of host presentation. Interlaced NTSC uses
  roughly 59.94 fields and 29.97 ordinary gameplay updates per second.
  An opt-in fixed-frame mode measured 29.73–29.97 game updates/s with roughly
  80–90 graphics frames/s in the opening area on native Windows. It uses
  isolated, conservative geometry interpolation; combat and physics remain
  on the original update loop. See [fixed-frame performance](docs/fixed-frame-performance.md)
  for launchers, timing, visual latency and Linux limits, and
  [optional MSVC PGO](docs/windows-pgo.md).
- Broader rendering accuracy remains unverified.
  Vulkan field presentation uses GPU bob to avoid temporal text combing.
  Audio hardware behaviour is
  approximate in places. Later areas, a full playthrough and saving/loading
  at a real save point remain unverified.
- Returning to the menu and starting another game could stall the intro.
  Movie recreation now clears stale callback registrations, and suspended
  input DMA preserves its active tag. Native Windows replay completed the
  second intro without skipping, then verified movement and inventory.

See [Windows validation](docs/windows-validation.md) for the tested compiler,
source-only regressions, capture evidence and practical limits. Earlier
Linux findings remain in [the work log](docs/NOTES.md).

## Build from your disc

For native Windows, follow [the Windows guide](docs/windows.md). It uses
Visual Studio 2022, a pinned PS2Recomp checkout and separate directories for
the tools, runtime build and private game files.

The existing Linux workflow remains available:

```sh
./scripts/00-build-tools.sh
./scripts/01-extract.sh "disc/King's Field The Ancient City.iso"
./scripts/02-recompile.sh
./scripts/03-build-runner.sh
./scripts/04-run.sh
```

Read [the Linux build guide](docs/building.md) first. Generated files default
to `~/.local/share/kfiv-pc`, outside this repository. The complete runner now
builds and the focused runtime/VU tests pass on Ubuntu under WSL2. WSL's
Dozen Vulkan driver failed before gameplay; see the separate
[Linux execution results](docs/fixed-frame-performance.md#ubuntu-and-the-owners-merged-work).

The tools build applies generator-affecting patches **before** generation.
Rebuild the recompiler and regenerate the complete output after updating
those patches, including `register_functions.cpp`.

## Contributing

Runtime and generator fixes are an ordered [patch series](patches/README.md)
of 58 patches on top of PS2Recomp commit `c5a9d02`. The existing 41 patches
are retained, including the owner's merged VU census/static-lift work.
The performance additions address GPU/SPU/VU overhead, hardware time,
cooperative VIF service, bounded native VU compilation and optional fixed
game updates with independent interpolated rendering. Diagnostics and the
separate experimental static-lift path remain off by default.
Read [the development
workflow](docs/contributing.md), [source-only tests](tests/README.md) and
[agent instructions](AGENTS.md). All game-derived output stays private.

## Layout

| Path | Contents |
|---|---|
| `kfiv/` | Recompiler configuration, Ghidra function map and exact boot ELF hash |
| `patches/` | Runtime, Windows build and generator fixes for the pinned base |
| `scripts/` | Linux workflow and shared safe patch application helper |
| `scripts/windows/` | Native Windows configuration, staging, rebuild, launch and test helpers |
| `tests/` | Synthetic source-only regressions and Windows audio capture diagnostic |
| `docs/` | Build guides, development workflow, validation and historical findings |

The repository tooling is MIT. PS2Recomp-derived patches and the tests are
GPL-3.0; the adapted RecompOne SPU implementation retains its MIT notice.

## Acknowledgments

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j, inspired by
  N64Recomp, with PCSX2 as a runtime reference.
- [Verdite](https://github.com/Voicedrew11/verdite3) by Voicedrew, whose
  RecompOne SPU implementation informed the working sound path.
- [ps2recomp-workbench](https://github.com/phmdacosta/ps2recomp-workbench)
  for the original Windows setup methodology.

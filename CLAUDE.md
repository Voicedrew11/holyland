# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

holyland is a native PC port (Windows, Linux; 64-bit) of King's Field IV, built from the
[kfiv-decomp](https://github.com/Voicedrew11/kfiv-decomp) decompilation of the PS2 game. The game's own
C is compiled as is (with patches); `port/` stands in for the PS2 beneath it. No game data is in the repo:
running needs the owner's disc image (`--disc <iso or folder>` or `KF4_DISC`).

`README.md` is out of date (it still says "step 0"). Trust the code and the git log.

## Build, test, run

```sh
git submodule update --init                # decomp/ and third_party/parallel-gs
cmake --preset linux-clang                 # or linux-gcc, windows-clang (llvm-mingw); output in out/<preset>/
cmake --build out/linux-clang
ctest --test-dir out/linux-clang           # all unit tests
ctest --test-dir out/linux-clang -R pcr_gif   # one test (names: add_test in CMakeLists.txt)
out/linux-clang/kf4 --disc <image> [--renderer native|gs|both|modern|fast|pc] ...
out/linux-clang/kf4 --help                 # every option
```

- The game code is GNU C: MSVC `cl` cannot build it.
- Sanitizers: `-DKF4_SANITIZE=address,undefined` (or `thread`). Use clang (`out/linux-clang-asan`,
  `out/linux-clang-tsan`); the gcc sanitizer builds don't link.
- Useful run options for testing without playing: `--headless` (needs `--dump-frames` to produce
  anything), `--teleport [VBLANK:]MAP[:PLACE]` with `--invincible`, `--input FILE` (`tests/*.pad`),
  `--frame-rate 240`, `--state-save-at`, `--gs-capture-at`.
- Many `KF4_*` environment variables switch debug paths and alternatives on and off (grep `getenv("KF4_`).
- No `.clang-format` or `.clang-tidy` in the project.

Test scripts in `tools/` (see `tools/README.md`): `boot_test.py`, `frame_test.py`, `pace_test.py`
(pictures a second at a few maps), `map_tour.py` (every map pictured), `screen_fx_test.py`,
`flash_find.py`. `kf4-gsreplay` (built with the game) replays a GS capture (`.kf4gs`, F9 in game).

## The decomp is read-only

- `decomp/` is a submodule pinned to one commit and must not be edited (configure fails if it is).
- PC edits to game code are patches in `patches/` (`patches/game/<folder>.patch`, `_top.patch`,
  `nonmatching.patch`). At configure, CMake copies the decomp, applies the patches, and compiles the copy
  in `out/<preset>/decomp_src/`. Compiler errors point there, but those files are read-only and
  regenerated each configure.
- To change game code: `python3 tools/patches.py apply` (builds the `work/` git copy), edit in `work/`,
  then `python3 tools/patches.py save` to write `patches/`. `upgrade` moves the decomp pin and carries the
  patches along.
- `tools/port_census.py` (what still stops each folder compiling) and `tools/port_abi_check.py`
  (declarations that disagree with definitions; these break on 64-bit even when they compile).
- Fixes to the reconstruction itself belong upstream in kfiv-decomp.

## Layout

- `game/`: new game-side features in C that live with the game code (quick save, automap, cheats,
  widescreen, draw distance, frame interpolation, screen effects, stats, weapon sets) and
  `game/native_draw/` (draws the game's models directly instead of through its VU1 packets).
- `port/platform/`: the PS2 under the game: kernel and threads (`fiber.c`), memory and EE addresses, DMA,
  VIF1, VU1 (interpreter `vu1.c` plus HLE per VU1 program `vu1_hle_*.c`), disc, save states, pacing
  (`refresh_clock.c`, `unlocked_rate.c`, `screen_vblank.c`), the Vulkan presenter, and `main_host.c` (options).
- `port/sdk/`: the PS2 SDK libraries the game links against (libgraph, libpad, libcdvd, libmc, libc/libm
  with PS2-exact results, MPEG). `port/include/`: their headers. `port/audio/`: the IOP sound side.
- `port/gs/`: the Graphics Synthesizer. Three implementations behind `port/platform/gs_api.h`, chosen
  by `--renderer`:
  - `gs_parallel.cpp`: paraLLEl-GS (`third_party/parallel-gs`), the accurate GS emulator (`gs`).
  - `native/` (`gsn*`): the native GS, a Vulkan renderer at higher resolution (`native`, `modern`,
    `fast`). Built as one translation unit, `gsn.cpp`, from many `.inc` files; also holds upscaling
    (FSR/DLSS).
  - `pc/` (`pcr*`): the PC renderer (`--renderer pc`), drawing the GS's input with its own code and
    none of the native GS's. It shares `gsn.h`'s interface and plain types. Current work on branch
    `pc/renderer`; the plan and where it stands are in `docs/port/PC_RENDERER.md`.
- `port/render/`: CPU-side records shared by the renderers (VU1 recording, part moves for pictures
  between game frames). Shared shader headers in `port/render/shaders/`.
- `port/overlay/`: the ImGui overlay (settings, stats panels, profiler, automap view).
- `cmake/granite_*.cmake`: patches written into paraLLEl-GS's Granite at each configure. The submodule
  shows as dirty because of them (`ignore = dirty`); that's expected.

## Concepts that span files

- The game runs at the PS2's rates (30/60 game frames a second); higher display rates draw
  "pictures between" game frames by moving the kept frame's vertices to the picture's time
  (`frame_interp.c`, `part_moves`, `pcr_between`).
- EE addresses stay 32-bit integers in game structs; the port maps them to host memory
  (`port/platform/ee_address.c`, `memory.c`).
- Save states are exact but tied to one build. Quick saves (F2/F4) are the game's own save and survive
  updates.

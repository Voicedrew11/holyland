# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Holyland (named Darker Reality from 2026-11-18 to 2026-11-20) is a native PC port (Windows, Linux; 64-bit) of King's Field IV, built from the
[kfiv-decomp](https://github.com/Voicedrew11/kfiv-decomp) decompilation of the PS2 game. The game's own
C is compiled as is (with patches); `port/` stands in for the PS2 beneath it. No game data is in the repo:
running needs the owner's disc image (`--disc <iso, chd or folder>` or `KF4_DISC`).

`README.md` is out of date (it still says "step 0"). Trust the code and the git log.

## Build, test, run

```sh
git submodule update --init                # decomp/ and third_party/parallel-gs
cmake --preset linux-clang                 # or linux-gcc, windows-clang (llvm-mingw); output in out/<preset>/
cmake --build out/linux-clang
ctest --test-dir out/linux-clang           # all unit tests
ctest --test-dir out/linux-clang -R pcr_gif   # one test (names: add_test in CMakeLists.txt)
out/linux-clang/kf4 --disc <image> [--renderer pc|gs] ...
out/linux-clang/kf4 --help                 # every option
```

- Release builds: the `*-release` presets (LTO, no frame pointers, `KF4_PLAYER_BUILD`: Depth fixed at Low) and `tools/pgo_train.py` (PGO); check one against
  the developers' build with `tools/build_compare.py` (`tools/README.md`, "A release build").
- The game code is GNU C: MSVC `cl` cannot build it.
- SDL3 (3.2 or newer; releases pin 3.4.20 in `packaging/toolchain.lock`): Fedora's `SDL3-devel`, or `-DSDL3_DIR=<its lib/cmake/SDL3>`
  (Windows: SDL3's MinGW build). Without it the game builds with no window, input or sound.
- Sanitizers: `-DKF4_SANITIZE=address,undefined` (or `thread`). Use clang (`out/linux-clang-asan`,
  `out/linux-clang-tsan`); the gcc sanitizer builds don't link. How to run them, TSan's suppressions
  (`tools/tsan.supp`) and the known reports: `docs/port/SANITIZERS.md`.
- Useful run options for testing without playing: `--headless` (needs `--dump-frames` to produce
  anything), `--teleport [VBLANK:]MAP[:PLACE]` with `--invincible`, `--input FILE` (`tests/*.pad`),
  `--frame-rate 240`, `--state-save-at`, `--gs-capture-at`.
- Many `KF4_*` environment variables switch debug paths and alternatives on and off (grep `getenv("KF4_`).
- No `.clang-format` or `.clang-tidy` in the project.

## Testing graphics: open a real window

When testing graphics (speed, GPU memory, GPU hangs, pacing), run in a real window with the owner's settings, not
only `--headless`. A headless run or one with a pad script (`--input`) does not read `settings.json`, simulates the
screen, and sizes things for 60 pictures a second: on 2026-11-13 headless runs at a simulated 240 Hz showed 3.7 GB of
GPU memory and no hang where the owner's real play (unlocked, cap 500) held 8 GB and hung the card.

- A scripted run reads settings with `KF4_READ_SETTINGS=1`. Point `KF4_DATA_DIR` at a copy of `~/.local/share/holyland`
  so the owner's files are not written. Not `KF4_OVERLAY_SHOT`: it reads settings too but hides the window.
- The window must be on the screen: the owner should be able to see the run.
- Leave out `--headless`; keep `--input tests/newgame.pad --teleport ... --invincible --no-audio --saves none`.
- Check the run's `[stats]` lines show what you meant to test (pictures between drawn, native draws not 0).
- Headless runs are still right for comparing pictures byte for byte (`--dump-frames`).

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
  widescreen, draw distance, frame interpolation, screen effects, stats, hands and hotkeys) and
  `game/native_draw/` (draws the game's models directly instead of through its VU1 packets).
- `port/platform/`: the PS2 under the game: kernel and threads (`fiber.c`), memory and EE addresses, DMA,
  VIF1, VU1 (interpreter `vu1.c` plus HLE per VU1 program `vu1_hle_*.c`), disc, save states, pacing
  (`refresh_clock.c`, `unlocked_rate.c`, `screen_vblank.c`), the Vulkan presenter, and `main_host.c` (options).
- `port/sdk/`: the PS2 SDK libraries the game links against (libgraph, libpad, libcdvd, libc/libm
  with PS2-exact results, MPEG). `port/include/`: their headers. `port/audio/`: the IOP sound side.
- `port/gs/`: the Graphics Synthesizer, behind `port/platform/gs_api.h`:
  - `scene/` (`scn*`): the scene renderer, the only one players get (the default since 2026-11-20). It runs
    under the PC renderer's shell and draws from the scene's records: `docs/plans/renderer-scene-not-gs.md`
    (the direction, the measurements behind it, and the design's handoff).
  - `pc/` (`pcr*`): the PC renderer the scene renderer replaces, now developers' only (`--renderer pc`). It
    draws the GS's input with its own Vulkan code on Granite: `docs/port/PC_RENDERER.md`.
  - `gs_parallel.cpp`: paraLLEl-GS (`third_party/parallel-gs`), the accurate GS emulator. Developers
    only (`--renderer gs`), to compare pictures against; never chosen by settings, the overlay or a
    fallback.
  - `shared/`: `gsn.h` (plain types and constants the PC renderer and the port share) and `gsn_vram.c`
    (the GS's memory formats). Left from the native GS, which was removed in November 2026 along with
    `--renderer native|modern|fast|both`, scene scale and FSR/DLSS; docs/ still describe it as history.
- `port/render/`: CPU-side records shared by the renderers (VU1 recording, part moves for pictures
  between game frames). Shared shader headers in `port/render/shaders/`.
- `port/ui/`: the player menu (F1), RmlUi in King's Field IV's look, its settings as data (`settings_rows.cpp`), and
  the first-start setup in the same window (`setup_pages.cpp`, shown once per `SETUP_VERSION`; `KF4_SETUP=1` forces it),
  drawn by the Vulkan presenter (`port/platform/present_vk_ui.cpp`): `docs/port/PLAYER_UI.md`, with the plan to
  retire ImGui. The launcher's window uses the same RmlUi code on SDL's renderer (`launcher/window.h`).
- `port/overlay/`: what ImGui still draws over the game (the automap's views, the extended stats), the settings file
  (`settings.c`); the developers' ImGui window (Shift+F1) was removed on 2026-11-24 (teleport is in Cheats).
- `launcher/`: **Holyland**, the launcher players get (its own CMake project, `cd launcher && cmake --preset
  linux`): builds the game on the player's computer from a release's source and shipped tools, then plays it;
  update notices. `packaging/` makes the releases (AppImage, Windows zip and installer), `.github/workflows/release.yml`
  on a `v*` tag. All in `docs/port/LAUNCHER.md`. The data folder is `holyland/`
  (on Windows too); the first start moves an old `darker-reality/` folder there (`port/platform/data_dir.c`).
- `third_party/vk-sync2-layer`: Khronos's synchronization2 layer, built into `vk_layers/` beside `kf4` and enabled only on
  a driver without `VK_KHR_synchronization2`, which Granite needs (`port/gs/pc/pcr_sync2.h`).
- `cmake/granite_*.cmake`: patches written into paraLLEl-GS's Granite at each configure (the list:
  `cmake/granite_edits.cmake`; a file is written only when its text changes, since a release's source is read-only). The submodule
  shows as dirty because of them (`ignore = dirty`); that's expected.

## Concepts that span files

- The game runs at the PS2's rates (30/60 game frames a second); higher display rates draw
  "pictures between" game frames by moving the kept frame's vertices to the picture's time
  (`frame_interp.c`, `part_moves`, `pcr_between`).
- EE addresses stay 32-bit integers in game structs; the port maps them to host memory
  (`port/platform/ee_address.c`, `memory.c`).
- Save states are exact but tied to one build. Quick saves (F2/F4) are the game's own save and survive
  updates.
- Mods (`port/platform/mods.c`, `docs/port/MODS.md`): `<data folder>/mods/<mod>/files/` (whole KF4.DAT members) and
  `text.json` (messages, item and spell names), read after the disc opens. `port_disc_replace_member` serves the new
  contents from memory past the disc's end and changes KF4.DAT's table as it is read; the game code is unchanged.
- No memory card: the save points' three saves are files (`<data folder>/saves/saveN.kf4save`, `--saves DIR|none`).
  `game/saves.c` stands in for the game's card layer (`save/save_card.c` and `save/mc.c` are left out of the
  build) and `port/platform/save_files.c` writes the files; the save and load screens skip choosing a card.

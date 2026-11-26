# macOS (Apple Silicon)

The game builds and runs natively on arm64 macOS (no Rosetta), on MoltenVK. Windows and Linux build exactly as
before: every change is behind `__APPLE__` / `__aarch64__` or `if(APPLE)`, checked below.

Status (2026-11-26, an M1 on macOS 27.2): all 57 unit tests pass; `tools/boot_test.py` passes; `tools/frame_test.py`
passes with the PC renderer and paraLLEl-GS, and the scene renderer's pictures are the same run after run; the game's
logs for every pad script in `tests/` are byte for byte those of an x86-64 build of the same code, in the developers'
build and the release preset's (LTO) alike; `tools/map_tour.py` loads and pictures all 64 maps; a scripted run in a
window draws and presents. Not yet tried by a player: a long play in a window (see "Not done").

## Building and playing

Xcode's command line tools and Homebrew:

```sh
xcode-select --install
brew install cmake ninja glslang sdl3 molten-vk python
git submodule update --init
git -C third_party/parallel-gs submodule update --init Granite
git -C third_party/parallel-gs/Granite submodule update --init third_party/volk third_party/khronos/vulkan-headers
cmake --preset macos-clang            # or macos-clang-release (LTO), as players' builds are
cmake --build out/macos-clang
out/macos-clang/kf4 --disc <image>
```

The game finds MoltenVK beside itself (`libMoltenVK.dylib`, as a packaged build would carry it) or Homebrew's
(`port/platform/vulkan_library_macos.c`). Its data folder is `~/.local/share/holyland`, as on Linux
(`port/platform/data_dir.c`'s POSIX rules; `~/Library/Application Support` would be the Mac's own place, a new
row there for the team to decide on).

## What macOS needs, and why

### The EE's float mode on ARM

The game's threads compute as the EE's FPU does: round toward zero, denormals flushed on the way in and out
(`port/include/port/platform.h`). On x86 that is MXCSR `0xFFC0` (RZ, FTZ, DAZ), which each fiber keeps as its own.
On arm64 it is FPCR `0x01C00000`: RMode 3 (toward zero) and FZ, which flushes denormal operands and results
alike, FTZ and DAZ together. In that rounding a result is tiny before rounding exactly when it is after, so where
x86 and ARM detect tininess (after and before rounding) differs nowhere here. `PORT_MXCSR_*` hold FPCR values on
arm64 (FPCR keeps no flags: those are FPSR's), and `port_host_float_begin/end` switch FPCR for host code
(printf, strtod), as they switch MXCSR on x86.

- `port/platform/fiber.c`: an AArch64 switch for arm64 macOS (Mach-O): x19..x30, d8..d15 and FPCR kept; x18 is
  Darwin's. A new fiber starts with the EE's FPCR in its frame.
- VU1 uses DAZ alone (denormal operands read as zero, results not flushed), which ARM has no mode for. Its code
  already had the way without (`VU_DAZ 0`, `port/platform/vu1_internal.h`): the operands are masked in software,
  which gives the same bits; the HLEs' fast paths that check MXCSR's flags are off there, so VU1 runs its exact
  paths.
- `game/automap.c`'s mode switch, and `tests/ripple_test.cpp` and `tests/vu1_record_test.cpp`, use FPCR or
  `fesetround` where they used MXCSR.

### Integer division

ARM's division never traps. INT_MIN / -1 gives INT_MIN and remainder 0, as the EE's DIV does (what
`port/platform/int_divide.c` makes of x86's trap). A division by zero gives 0, where ee-gcc's check stops the PS2 and
x86 traps: on arm64 macOS the compiler puts that check before each of the game's divisions
(`-fsanitize-trap=integer-divide-by-zero`, `KF4_GAME_FLAGS`), and `int_divide.c` reports its trap (`brk #0x5503`)
as x86's: "integer division by zero at ...: the PS2 stops at this division too".

### The program's image

- EE addresses for the program's globals come from its Mach-O segments (`port/platform/ee_address.c`), as from
  Linux's PT_LOADs; none of the port's address model depends on where the image is.
- Mach-O wants `segment,section`: the decomp's few `__attribute__((section(".bss")))` (kept from its matching) are
  given Mach-O names in the staging copy at configure (CMakeLists.txt, Apple only): `.bss` and `.data` in `__DATA`,
  `.rodata` in `__DATA_CONST,__const` (its tables hold pointers, which a position-independent program relocates).
- arm64 macOS links every program position independent, with ASLR: save states (which need the program at a
  fixed address) are off there and say so (`port/platform/savestate.c`). Quick saves work. Save states could come
  back by starting the game again without ASLR (`posix_spawn`'s `_POSIX_SPAWN_DISABLE_ASLR`, which works for an
  unsigned or ad hoc signed program).
- `port/platform/kernel.c`'s `signal` (a semaphore's) is named `kernel_sema_signal` on macOS: macOS's `<stdlib.h>`
  declares the C library's.
- Thread names: `pthread_setname_np(name)`, macOS's form (`port/platform/thread_name.c`).

### Vulkan: MoltenVK

- Granite (the device) and SDL (the window's surface) must load the same Vulkan library, and look where macOS has
  none: `port/platform/vulkan_library_macos.c` gives both (`GRANITE_VULKAN_LIBRARY`, `SDL_VULKAN_LIBRARY`) the
  first of `libMoltenVK.dylib` or `libvulkan.1.dylib` beside the program, then Homebrew's loader, then its MoltenVK.
- The synchronization2 layer is not built: MoltenVK has `VK_KHR_synchronization2` itself (and the layer is an ELF
  one).
- Granite's timer: macOS has no `clock_nanosleep` (`cmake/granite_macos.cmake`, made with the other Granite edits;
  code for `__APPLE__` only).
- **The shaders.** MoltenVK's SPIR-V to Metal (SPIRV-Cross) turns `gl_SampleMaskIn` read in a function other than
  `main()` (`pcr_prim.h`'s `trace()`) into Metal that does not compile: the function takes an array, the call
  passes a number. With `PCR_MOLTENVK`, `main()` reads it into a global that `trace()` reads. macOS's build makes
  the renderers' shaders with that define, into its build folder (`tools/build_shaders.py --define PCR_MOLTENVK
  --pc-out --scene-out`, CMakeLists.txt), and takes them instead of the committed `pcr_shaders.h` and
  `scn_shaders.h` (`KF4_MACOS_SHADERS`). So a macOS build needs `glslangValidator`. Without the define the shaders
  compile to the same SPIR-V as before (checked with one glslang).
- MoltenVK warns "Metal does not support disabling primitive restart": harmless unless a draw's indices use the
  largest value for a vertex (none showed in the pictures).

### The launcher

`launcher/platform/macos.cpp`, from Linux's: processes, libcurl (`/usr/lib/libcurl.4.dylib`), the system's file
chooser (osascript). It builds with Xcode's and Homebrew's tools (no release ships macOS tools yet) and does not
update itself (no `.app` is packaged). It builds, and its tests pass; its build of the game from a release's
`content/` was not tried.

## How it was checked

- **The same game as on x86.** An x86-64 build of the same code (the address of everything as on Linux, run under
  Rosetta) and the arm64 build ran every pad script in `tests/` and `tests/motion/` with `boot_test.py`'s flags
  (`--fast --headless --clock ... --saves none --no-audio`): the logs (each vblank's events, the map, the player's
  place and heading, HP) are byte for byte the same, about 37,000 vblanks in all: New Game, walking, twin-stick,
  menus, casting, combat, a kill, the herb, a cinematic.
- **Pictures.** `frame_test.py --renderer pc` and `--renderer gs` pass (the same pictures every run, a dense stretch
  of the opening, a read-back leaving nothing behind). The scene renderer's pictures at the same vblanks are the same
  run after run, and show the title and the first map as they should.
- **Every map.** `tools/map_tour.py --renderer scene --positions 2`: all 64 maps load, 8 of 8 views each, no crash
  and no MoltenVK error. Its "unusual" notes (some views dying, flat or black where the made-up place has no floor or
  no light, a few players placed nearby) want comparing with a Linux tour (`tools/tour_compare.py`); the pictures
  looked at were right. With `--gsreplay out/macos-clang/kf4-gsreplay` (map_tour looks for it in the Linux and
  Windows build folders only) six maps' captures replayed through VU1 again with no packet different.
- **A window.** A scripted run (`--input tests/newgame.pad`, no `--headless`) made its swapchain on the game's device
  and presented to the end. Its speed was not judged: the screen was asleep through the night, and macOS holds back
  the pictures of a window no one can see (the run's `[stats]` show it waiting to present).
- **Windows and Linux unchanged.** Every changed C, C++ and shader file, resolved with `unifdef` as Linux and as
  Windows (x86-64) compile it, is the same text at the base commit and now, but `pcr_prim.h`, whose one change
  (`PCR_SAMPLE_MASK_IN`) compiles to the same SPIR-V; the committed SPIR-V headers are unchanged. Granite's
  `util/timer.cpp`, as edited, resolves the same for both. The CMake changes are inside `if(APPLE)` / `if(NOT APPLE)`;
  `tools/build_shaders.py` writes what it wrote without its new options. The new files
  (`port/platform/vulkan_library_macos.c`, `cmake/granite_macos.cmake`, `launcher/platform/macos.cpp`) are built on
  Apple only.

## Not done

- A long play in a window. A first one (2026-11-26, the M1, release preset, scene renderer, fullscreen 1680x1050 with
  FIFO): the sound and the pictures right, the setup, the settings and the hands saved, 30 to 60 pictures a second
  and never under 30, with a few hitches of 80-180 ms as the first start made its pipelines.
- Speed: not measured (see "A window"). VU1 runs its exact paths there, without the fast ones that check MXCSR's
  flags (FPSR's could stand in).
- The first start makes its pipelines without a seed: `port/gs/pc/pipeline_seed.bin` is of the committed shaders,
  and macOS's differ (PCR_MOLTENVK), so they are made before and during play the first time (MoltenVK compiles them
  to Metal then) and kept in the data folder after.
- Packaging (an `.app`, MoltenVK beside the game, signing and notarizing) and a release's macOS tools for the
  launcher.

## Next: speed on the Mac

The first play on an M1 ran at 30 to 60 pictures a second (above). In the order they would likely help:

1. **A pipeline seed for macOS's shaders.** `port/gs/pc/pipeline_seed.bin` is of the committed SPIR-V; macOS's
   (PCR_MOLTENVK) differ, so its first start makes pipelines in play (80-180 ms hitches, MoltenVK compiling to
   Metal). Make a seed on a Mac (`tools/pipeline_seed.py`, as for the committed one) and carry it for macOS builds;
   or move `trace()`'s workaround into the committed shaders once the team agrees, so one seed serves all.
2. **VU1's fast paths on arm64.** `VU_DAZ` is 0 there, so the HLEs (`vu1_hle.c`, `vu1_hle_280.c`) and
   `port/render/vu1_record.cpp` run their exact paths only. Their fast paths check MXCSR's sticky flags to know the
   fast result was exact; FPSR (IOC, OFC, UFC, IDC) can stand in, with FPCR's FZ for FTZ+DAZ. VU1 took about 10 s of
   a 150 s run in the window.
3. **Measure first.** `--stats` and `tools/pace_test.py` in a visible window (a hidden or sleeping screen holds back
   presentation), at the player's own settings (`KF4_READ_SETTINGS=1`), to see whether the CPU (VU1, the GIF hand-off)
   or the GPU (the scene renderer's passes at the internal resolution) is the bound.
4. **The GPU side.** The scene renderer drew 1920x896 scaled to the window: the internal resolution, anti-aliasing
   and the depth mode (exact Z, decals) are the M1 GPU's biggest costs. MoltenVK's knobs (`MVK_CONFIG_*`: Metal argument
   buffers, synchronous queue submits off) are worth a try once the bound is known.
5. **Fragment shader interlock.** The PC renderer's depth rule uses it (MoltenVK maps it to Metal's raster order
   groups): measure its cost on Apple GPUs against the depth modes without it.

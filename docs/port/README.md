# The PC port

holyland is a native PC port of King's Field IV built from the decomp's source. The game code is the
decomp's, pinned in `decomp/` and never edited here; the PC edits to it are kept as patches in
`patches/`. Under it, `port/` stands in for the PlayStation 2. The approach follows
[ico-pc](https://github.com/nathanialf/ico-pc), the PC port of the ICO decomp:
- The game code is compiled for the PC as 64-bit C, with nothing emulated or recompiled.
- Where the code relies on the PS2, it is changed to the PC form, by the rules in
  [RULES.md](RULES.md).
- `port/` provides what the PS2 did, keeping the SDK's names so the game code reads as before:
  - game RAM;
  - the hardware registers;
  - the Sony libraries;
  - later the renderer, the sound driver and the platform layer.

## The decomp and the patches

`decomp/` is a git submodule of [kfiv-decomp](https://github.com/Voicedrew11/kfiv-decomp.git), pinned
to one commit (currently `88b0978`).

> **The decomp is read-only here.** Never edit `decomp/` or the compiled copy in
> `out/<preset>/decomp_src/`; this goes for people and agents alike. Configure fails if `decomp/` has
> local changes, and `decomp_src/` is made read-only (an edit there would be lost at the next
> configure). Edit game code in `work/` and save it into `patches/` (below).

Clone with submodules, but not recursively: `third_party/parallel-gs` (the GS renderer) has a
Granite submodule with twenty more of its own, and the port needs only two of them.

```sh
git clone <this repository> && cd holyland
git submodule update --init
git -C third_party/parallel-gs submodule update --init Granite
git -C third_party/parallel-gs/Granite submodule update --init third_party/volk third_party/khronos/vulkan-headers
```

Without the last two lines the port still builds, but draws nothing.

The PC edits are in `patches/`, as diffs against the decomp with paths relative to its root:
- `patches/game/<folder>.patch`: one per top-level folder of `src/game`.
- `patches/game/_top.patch`: the files directly in `src/game`.
- `patches/nonmatching.patch`: the map draw `src/nonmatching/func_00112BD0.c`, which the PC build
  compiles.

Game code that exists only on the PC, with nothing in the decomp to patch, is ordinary files in
`game/`: `game/data/` holds the data sections that the decomp still has in asm, defined in C. It is
compiled with the patched decomp and edited in place.

The decomp's own tooling and docs (its PS2 build, `build.sh`, `tools/match.py`, the splat config) live
in kfiv-decomp. They do not apply to the patched game code here.

Fixes that are not PC-specific (names, types, structs, matching) go to kfiv-decomp first, and come
here with a pin move (see below).

### Changing game code

1. `python3 tools/patches.py apply`: makes `work/` (git-ignored) a fresh copy of the decomp with the
   patches applied. `work/` is its own git repository, and its `pristine` tag is the unpatched decomp.
   If `work/` has changes that were not saved, `apply` refuses; `apply --force` discards them.
2. Edit the game code in `work/src/game`, by the rules in [RULES.md](RULES.md).
3. `python3 tools/patches.py save`: regenerates `patches/` from `work/`. Commit `patches/`.
4. `cmake --build out/<preset>`: since the patches changed, configure runs again on its own. It
   copies `decomp/src/game` and `decomp/src/nonmatching`, applies `patches/` to the copy, and updates
   `out/<preset>/decomp_src/` with the files that changed, so only those recompile. The build compiles
   `decomp_src/` with `game/` and `port/`. Configure fails if `decomp/` has local changes.

### Moving the pin

`python3 tools/patches.py upgrade <commit>` moves the submodule to a newer kfiv-decomp commit:
- it follows file renames (`git diff -M` between the two pins) and function and global renames (from
  `config/symbol_addrs.txt`, whose addresses are stable);
- it re-applies the patches with `git apply -3` and reports conflicts.

It does not follow renames of struct fields or types: `symbol_addrs.txt` has only functions and
globals. Those come up as conflicts, or as compile errors after a clean upgrade, and are fixed by
hand.

If that is clean, it moves the submodule and rewrites `patches/`. Then commit `decomp` and `patches/`.
If not, it keeps the scratch copy so the conflicts can be resolved there, and then
`python3 tools/patches.py save --from <that dir>` writes the result to `patches/`.

## Building

The game code is GNU C, so build it with clang or gcc (not MSVC's cl). You need CMake 3.21 or newer
and Ninja. The renderers, the native GS (`port/gs/native/`, the default) and paraLLEl-GS (the
reference), are C++ (clang++ or g++, which the presets name) on Vulkan, through paraLLEl-GS's Granite.
They run on desktop Vulkan GPUs (paraLLEl-GS's README lists the features it needs); running them needs
a Vulkan driver, building them does not.

**Windows:** use [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), or LLVM's clang, on the PATH:

```sh
cmake --preset windows-clang -DSDL2_DIR=C:/path/to/SDL2-2.30.9/x86_64-w64-mingw32/lib/cmake/SDL2
cmake --build out/windows-clang
```

SDL2 on Windows comes from its mingw development package (`SDL2-devel-2.x.y-mingw.tar.gz` from
[SDL's releases](https://github.com/libsdl-org/SDL/releases)); `SDL2_DIR` points into it. The build
copies `SDL2.dll` and llvm-mingw's `libc++.dll` and `libunwind.dll` next to `kf4.exe`.

**Linux:**

```sh
cmake --preset linux-gcc        # or linux-clang
cmake --build out/linux-gcc
```

The program is `out/<preset>/kf4`. SDL2 is used for the window and the pad if CMake finds it
(`-DKF4_USE_SDL=OFF` turns it off). The renderers are built when paraLLEl-GS is checked out
(`-DKF4_USE_PARALLEL_GS=OFF` turns them off); without them the game runs and draws nothing.

## Running

```sh
out/linux-gcc/kf4 --disc kfiv_us.iso
```

| Option | What |
|---|---|
| `--disc PATH` | the game's disc image (or set `KF4_DISC`) |
| `--fast` | run as fast as possible instead of at 60 vblanks a second (59.94: each vblank waits for the host's clock, on Windows a high-resolution waitable timer; the window shows each vblank's picture, waiting up to half a vblank for it; one later than that is shown at the next vblank, and a window still a vblank behind at the next present catches up by leaving one out) |
| `--headless` | no window, no SDL; the GS draws only for `--dump-frames` and GS captures (`--gs-capture-at`, `KF4_DEBUG_AT`) |
| `--no-gs` | draw nothing (VIF1 and VU1 still run) |
| `--renderer native\|gs\|both\|modern\|fast` | who draws: the native GS (the default; step 3, [STEP3.md](STEP3.md)), paraLLEl-GS (the reference, kept for comparison; it draws instead if the native GS cannot start), or both side by side with their pictures compared each vblank (`--renderer-shown native` shows the native one, `--renderer-shown heatmap` where the two differ; `--stats` prints the comparison). The Debug page sets it for the next start (`settings.json`, `display.renderer`). `fast` is `modern` with the PC renderer's parts built so far (PC_RENDERER.md; P1: `--native-sampler hardware`) |
| `--native-scale auto\|N` | the native GS draws at N times the PS2's resolution, 1 to 4, or `auto` (the default): the multiple of 448 lines nearest the height the window shows the picture at (1080: 2, 1440: 3, 2160: 4; at most 2 on an integrated GPU; 1 headless) (live on the Debug page; `settings.json` `display.native_scale`, a number or `"auto"`) |
| `--native-widescreen`, `--no-native-widescreen` | with `--widescreen`, the native GS draws the 16:9 picture with square pixels: its frame buffers 4/3 of `--native-scale` across, rounded up (live on the Debug page; `settings.json` `display.native_widescreen`, on by default there; off on the command line alone) |
| `--flicker-filter` | draw the game's flicker filter, as on the PS2: every frame of play it blends each row of the picture half with the row above, so that thin lines do not flicker between an interlaced TV's two fields. Off by default: on a PC screen it blurs the picture up and down, and above 1x shows it twice a GS row apart (the Display page's "No flicker filter"; `settings.json` `display.flicker_filter`) |
| `--native-msaa N` | the native GS draws the 3D with N samples a pixel (1, the default: off; 2, 4 or 8), smoothing its edges (on the Debug page, live but for a change of count, which takes a restart; `settings.json` `display.native_msaa`) |
| `--native-aniso N` | anisotropic filtering of the native GS's mipmapped textures: up to N reads along a pixel's footprint (1, the default: off; 2, 4, 8 or 16), for floors and walls at a grazing angle (live on the Debug page; `settings.json` `display.native_aniso`) |
| `--native-sampler gs\|hardware` | how the native GS's scene reads its textures: `gs` (the default), the GS's texture unit in the shader, texel by texel; `hardware`, the GPU's sampler on RGBA8 copies of the textures with their mip levels (its filtering and anisotropy: faster, the pictures differ in filtering only). `--renderer fast` takes `hardware` unless this says otherwise (PC_RENDERER.md, P1) |
| `--native-colour exact\|high` | how precisely the native GS keeps colours: `exact` (the default), the GS's 8 bits a channel and its roundings; `high`, 16 bits a channel and the colours' arithmetic without the roundings, the picture dithered to 8 bits, so smooth dark gradients (the sky, glows) have no bands (on the Debug page, live; `settings.json` `display.native_colour`; PLAN.md) |
| `--native-shine whole\|exact` | with `--renderer modern`, the ice creatures' sphere-mapped shine (map 017, blended in a shader): drawn whole (the default), or exact, in runs, as the PS2 blends it where a part folds over its own edge: a few pixels a picture brighter along those edges, and much slower there (about 140 pictures a second at 240 against 220-240) (live in the Debug page's Renderer section, "Ice shine: exact PS2 folds (slower)"; `settings.json` `display.native_shine`, `"whole"` or `"exact"`; NATIVE_DRAW_P8.md, "The ice shine drawn whole") |
| `--presenter vulkan\|sdl` | how the picture reaches the window: a Vulkan swapchain on the native GS's device, the native GS's picture never leaving the GPU (the default; `port/platform/present_vk.cpp`), or SDL_Renderer. Vulkan needs the native GS (`--renderer native` or `both`): with paraLLEl-GS alone the window is SDL_Renderer's, as it is if the swapchain cannot be made. The Debug page sets it for the next start (`settings.json`, `display.presenter`) |
| `--vu1 interp\|hle\|check` | who runs the VU1 programs (0x000, 0x280, 0x5A0, 0x700; 0x276 passes packets through): `hle` (the default), computed directly with the interpreter's results to the bit (`port/platform/vu1_hle*.c`), or `interp`, the cycle-accurate interpreter; `check` runs both and compares them run by run (`--stats` counts the differences) |
| `--no-deinterlace` | show each field as the GS scans it out (224 lines), not the two fields woven into one picture |
| `--no-progressive` | read a frame-mode interlaced picture a field at a time |
| `--no-anti-blur` | keep the blur of a game that blends its two read circuits |
| `--no-adapt-width` | scan out at the TV's width, not the game's |
| `--no-whole-frame` | let the game draw its picture between over every other line while the view turns (`Game_ShiftScreenByViewTurn`, for the other field of an interlaced TV): the picture is then read a field at a time and deinterlaced; and show the movies as two fields, woven with a comb wherever the picture changes; and weave each picture of the menus (drawn a field a vblank) with the one before; as on the PS2 (docs/port/PLAN.md) |
| `--no-seamless-scroll` | draw the scrolling textures (the sky's clouds on map 000, the lava, the water) as the PS2 does, with a sprite that stops half a pixel short of its 128x128 target: the texture then does not repeat evenly, a line where the clouds or the lava warp, clearer at a larger `--native-scale` (`port_seamless_scroll()`, PLAN.md) |
| `--ps2-scanout` | all six of the above, and `--flicker-filter`: the GS's raw output, for comparing with the PS2 or PCSX2 |
| `--dump-frames N,N,...` | write the picture shown at these vblanks as `frame_NNNNNN.png` |
| `--dump-dir DIR` | where `--dump-frames` writes (the current directory by default) |
| `--frames N` | stop after N vblanks |
| `--input FILE` | play the pad from a script instead of SDL |
| `--log FILE` | the per-frame log (`-` for stdout) |
| `--clock YYYY-MM-DDTHH:MM:SS` | the disc drive's clock at boot, in Japan's time, instead of the host's; it runs on with the game's time, for repeatable runs |
| `--host-dir DIR` | the folder `host0:` paths are read from |
| `--controls ps2\|verdite` | the keyboard and mouse layout (see below) |
| `--audio-dump FILE` | write the sound as a 48 kHz WAV file |
| `--no-audio` | open no sound device (the sound is still made) |
| `--stats` | at exit, print where the DMA controller's thread spent its time: VU1 (its runs and instructions too), handing GIF packets to the GS, and vsyncs; and a hash of every packet VU1 sent, which two runs that drew the same share. For A/B timings. It counts from the last save state saved or loaded |
| `--state-save-at VBLANK[:SLOT\|:FILE],...` | save a state at these vblanks, to a slot (0 when left out) or a file (see "Save states") |
| `--state-load [VBLANK:]SLOT\|FILE` | load a state before the game starts, or once the run reaches that vblank |
| `--quick-save-at VBLANK[:SLOT\|:FILE],...` | take a quick save (slot 0 when left out) at these vblanks, or as soon after as the player is in control (see "Quick saves") |
| `--quick-load [VBLANK:]SLOT\|FILE,...` | load a quick save on the title screen, or in play once the run reaches that vblank; several (in vblank order) are loaded one after another, as `tools/map_tour.py` does |
| `--invincible` | debugging: the player loses no HP and does not die (the Debug page's Invincible) |
| `--cheats spells,equipment,items,magic\|all` | debugging: once in play, as the Debug page's cheat buttons do (`game/cheats.c`) |
| `--teleport [VBLANK:]MAP[:PLACE]` | once in play (from that vblank), put the player in map MAP at its PLACE-th place (0, the map's start, when left out), keeping the game's progress, as the overlay's Teleport does (see "Teleport"); with a vblank it is `VBLANK:MAP:PLACE` |
| `--memcard DIR\|none` | the folder that is the memory card in slot 1 (by default `memcard` in the data folder, beside settings.json); `none` leaves the slot empty, as the tests do |
| `--record-input FILE` | write what the pad and keyboard send as a pad script, to replay with `--input` |
| `--widescreen` | the opt-in 16:9 picture (also on the overlay's Display page; off by default): the game's view is widened, the menus, message boxes and HUD keep their 4:3 shape, and the window shows it at 16:9 (see "Widescreen") |
| `--widescreen-hud-centred` | the same, with the HUD in the 4:3 middle instead of the 16:9 picture's corner |
| `--present-scaling sharp\|smooth\|best`, `--present-sharpen X` | how the picture is fitted to the window: nearest, bilinear, or best (the default: a cubic filter on the GPU, area-weighted when the picture is larger than the window, sharper than bilinear when it is smaller); and an optional contrast-adaptive sharpening, 0 to 1 (also on the overlay's Display page, live; `settings.json` `display.scaling`, `display.sharpen`). `--dump-present WxH` makes `--dump-frames` write the picture as a window of that size shows it |
| `--frame-rate 30\|60\|120\|144\|240\|N\|monitor` | the opt-in higher frame rates (also on the overlay's Display page; 30, the PS2's, by default): in play, pictures between the game's frames, the camera, the models, the HUD and the scrolling textures where they are at each picture's time; `monitor` is the screen's refresh rate (see "Higher frame rates") |
| `--draw-distance 1\|1.5\|2\|4\|max` | the opt-in longer view (also on the overlay's Display page, live; 1, the PS2's, by default): the fog and the far plane that many times as far, max without distance fog (see "Draw distance") |
| `--draw-distance-sky ps2\|push` | with `--draw-distance`, the sky keeps the PS2's fog (the default) or has it pushed out too |
| `--myus-necklace` | the opt-in mod (also on the overlay's Mods page; off by default): the effect of Myu's Necklace always, worn or not (see "Mods") |
| `--separate-run` | the opt-in mod (also on the overlay's Mods page; off by default): run on a key of its own, not the action button (see "Mods") |
| `--extended-stats` | the opt-in mod (also on the overlay's Mods page; off by default): what the game keeps and never shows, in its own menus, and the hit log (see "Mods", [EXTENDED_STATS.md](EXTENDED_STATS.md)) |
| `--automap`, `--automap-cheats` | the opt-in automap mod (also on the overlay's Mods page; off by default): a map of where you have been, M opens it; `--automap-cheats` with all its cheats on (see "Mods", [AUTOMAP.md](AUTOMAP.md)) |
| `--weapon-sets` | the opt-in mod (also on the overlay's Mods page; off by default): a second weapon and shield, swapped with the ones in hand on R (see "Mods", [WEAPON_SETS.md](WEAPON_SETS.md)) |
| `--show-fps` | the frame-rate counter on for this run (F3, or the overlay's Interface page; off by default; see "The frame-rate counter (F3)") |
| `--gs-capture-at VBLANK[:N],...` | write a GS frame capture of N vblanks (2 when left out) from each of these vblanks, as F9 does (see "Debugging") |
| `--flash-recorder off\|on\|SECONDS[,HEIGHT]` | the flash recorder (F8; on by default: the last 2 seconds of pictures shown, at 540 rows; also on the overlay's Debug page, not saved; see "Debugging") |
| `--fpu-round zero\|nearest` | how the game's float maths rounds: as the EE's FPU (`zero`, the default: toward zero, denormals flushed; RULES.md, "PS2 numbers") or as the PC's IEEE floats (`nearest`), for comparing |

The window shows the game at 4:3 (16:9 with widescreen). A game controller is the pad; so is the keyboard (the PS2 layout,
the default):

| Keys | Pad |
|---|---|
| arrows | d-pad |
| W A S D, I J K L | left stick, right stick |
| Z or space, X, C, V | cross, circle, square, triangle |
| Q, E, 1, 3 | L1, R1, L2, R2 |
| Enter, Backspace | start, select |

The Verdite layout (`--controls verdite`, or the overlay's Controls page) is that of the KF2/KF3 ports:
W A S D walk and side-step, the mouse turns and looks once captured (Escape), Space attacks
(`port/sdk/controls.c` lists it). Outside play (menus, pause, talking, the King's Map) A / D press
d-pad left / right and the Left / Right arrows press L1 / R1, so A / D move left and right in the menus as
W / S move up and down (the equipment screen's items, a list's pages, the options' values) and the
arrows do the shoulder buttons' jobs there (turning the weapon on the equipment screen, zooming the
King's Map); "A / D move left / right in menus" on the Controls page (`controls.menu_side_keys`) turns
it off. It follows the bindings (the keys of L1 / R1 and of d-pad left / right) and leaves the
controller alone. In both, F1 opens the settings overlay, F2 takes a quick save and F4
loads it (the saves to play with, which survive updates), F5 saves a save state to the quick slot and F7
loads it (exact, for debugging, same build only), F3 shows or hides the frame-rate counter, F8, F9, F10
and F12 are the debugging tools (see "Debugging"), F11 switches full screen, and Shift+P shows or hides the profiler (see "The
profiler"; it is not rebindable). Every key, mouse button and controller button can be rebound.

### The settings overlay (F1)

F1 opens a menu bar and the Settings window over the game, drawn with Dear ImGui
(`port/overlay/overlay.cpp`). While it is open the keyboard, the mouse and the controller go to it,
not to the game; the game runs on behind it unless "Pause the game while the overlay is open" is on.
F1 or Escape closes it. What is held as it closes does nothing in the game until it is let go.

| Page | What |
|---|---|
| Display | window size (common heights: 720, 900, 1080, 1440, 2160, or the PS2's 448 and 896 lines, with the width following 4:3 or 16:9; or a monitor's own size: 2560x1440, 2560x1600, 3440x1440, 3840x2160 or 3840x2400, with the picture fitted inside; sizes bigger than the screen are left out), full screen (borderless at the monitor's own resolution; F11), picture shape (4:3 or stretched), widescreen (16:9) and where its HUD goes, live, pictures a second in play (30 to 240, or the screen's rate), live, the draw distance and the sky's fog, live, the scaling to the window (sharp, smooth or best) and sharpening, vsync, and the five switches of the PC picture (deinterlace, progressive, anti-blur, adapt width, whole frames, seamless scrolling textures), live, for comparing with the GS's raw output |
| Audio | volume and mute (of what you hear: the game's sound and `--audio-dump` are unchanged) |
| Controls | the layout (PS2 or Verdite), every action's two keys or mouse buttons and its controller button (click a slot and press; Delete or a right click clears it), Verdite's mouse sensitivity, invert Y, the mouse buttons in menus and A / D in menus, and Defaults |
| Save states | Quick saves (for playing): the ten slots, which one F2 and F4 use, what each holds (map, level, HP, when), save, load and what the last request did (`port/include/port/quicksave.h`). Save states (for debugging): the ten slots (0 is the quick slot of F5 and F7), save, load, and what the last request did (`port/include/port/savestate.h`) |
| Mods | the opt-in changes to the game, live: Myu's Necklace effect without wearing it, and the automap's "Map" section (see "Mods") |
| Debug | pictures shown (the window's presents of a new picture, by either thread), vblanks and game frames a second; with `--stats`' counting switched on, the DMA controller's time a second; the map, position, heading, HP, MP, level and gold; a button that copies them for a bug report; the cheats for testing (Invincible, and buttons that learn every spell, add every piece of equipment and the carried items but the keys, and put MP and Magic Power at 999: `--cheats`, `port/include/port/cheats.h`); Teleport (see "Teleport"); and the debugging tools: a button for each, the captures folder and the last file written (see "Debugging") |
| Interface | pause while open, the frame-rate counter (F3), the overlay's size, and where `settings.json` is |

Every way the PC differs from the PS2 is a switch that starts as the port has it.

### The frame-rate counter (F3)

F3 (rebindable on the Controls page: "Show / hide the frame rate"), the Interface page's
"Frame-rate counter" or `--show-fps` shows the pictures the window presents a second in the picture's
top right corner, with the overlay closed, on a dark square sized to the picture (about 28 pixels high
at 1440); below it, when they differ, the game's own frames a second (`144 FPS` over `30 GAME` at
`--frame-rate 144`: the game runs at 30, the pictures between are the PC's; the menus, the title and
the movies, whose loops the play's frame count does not count, show their pictures alone). Both are sampled twice a
second (`port_video_pictures_per_second`, `port_video_frames_per_second` in `port/platform/video.c`):
the pictures counted are the new ones the window presented, by the game's thread or the pictures
between's own, so it reads true at 144 and 240. The presenters draw it themselves as plain squares
(the Vulkan presenter clears them into the swapchain's image), not with ImGui, so turning it on
leaves the pictures between on their own thread and their pacing as it was. It is kept in
`settings.json`'s `overlay.show_fps`; the mouse indicator moves below it while it is shown.

### The profiler (Shift+P)

Shift+P shows or hides a panel, over the game with the overlay closed, that says what holds the frame
rate down. Twice a second it reads counters every thread adds to while it is open
(`port/include/port/profile.h`, `port/platform/profile.c`; the panel `port/overlay/profiler.cpp`):

- the pictures the window shows a second against those it could (the frame rate, at least 60), the
  pictures the native GS draws and the game's vblanks a second;
- **Bound by**: the GPU (its share of the time at 80% or more: its times leave out the gaps between
  command buffers), or a thread busy 85% of the time or more (the game's, the DMA controller's, the
  native GS's or the back end's, its waits left out), the one furthest over; "stalls now and then"
  when it does not keep up and nothing is that busy; nothing when it keeps up;
- the GPU's time a picture against its budget (a second over the frame rate);
- each thread's busy share of a core, with what it waited for: the game's (its disc reads; not its
  wait for the vblank), the DMA controller's (VIF1, VU1 and the GIF; its waits for room in the GS's
  ring), the native GS's (the pictures between; its waits for the back end and the GPU), the back
  end's (its waits for the GPU at a frame context), the pipelines made while a draw waited, and the
  Vulkan presenter's waits for its fence and a swapchain image (vsync, or the GPU);
- the pipelines made in play since the start (by a draw that waited): how many, their time in all and the
  longest, counted whether the panel is open or not (`--stats` prints the same in `pipelines made ahead`);
- the pictures shown a second over the last half minute;
- the last eight stalls, a game vblank or a GS thread's vblank over two vblanks' time, with what it did
  (disc reads; pipelines made, waits for the GPU, pictures between, texture decodes and uploads).

"Copy report", with the settings open (F1: the mouse is the overlay's), puts the same as text on the
clipboard. Open, it costs the GPU its timestamps (`KF4_GSN_GPU_TIME=1`'s, around each render pass and
copy) and each thread a clock read around its work; closed, nothing but a check. The SDL_Renderer
presenter's waits are not measured.

**settings.json** keeps the settings, in the user's data folder: `%APPDATA%\holyland` on Windows,
`$XDG_CONFIG_HOME/holyland` (or `~/.config/holyland`) elsewhere; `KF4_DATA_DIR` names another folder.
The overlay's window places go beside it in `overlay.ini`. It is written a second after a change, when
the overlay closes and at exit; keys it does not know are kept, and a file that is not valid JSON is
reported and left alone (the defaults are used). The controls keep only the bindings that differ from
the layout's defaults, as `"key:Space"`, `"mouse:left"`, `"pad:a"` or `"trigger:lefttrigger"`.
`"version"` is 2 since the native GS and the Vulkan presenter became the defaults: every save writes
every setting, so an older file says `"renderer": "gs"` and `"presenter": "sdl"` whether the player
chose them or not. In a file without a version those two values are taken as the old defaults and the
new ones are used (the file is written again as version 2, with a line on stderr); `"native"`,
`"both"` and `"vulkan"` are kept. From version 2 on, `"gs"` and `"sdl"` are kept as chosen.
`"version"` is 3 since `"native_scale": "auto"` and `"native_widescreen": true` became the defaults
(October 2026): in an older file `"native_scale": 1` and `"native_widescreen": false` are taken as the
old defaults written out, and the new ones are used (the file is written again as version 3); other
scales, and native widescreen on, are kept.

Options on the command line override it for that run, and are not written back: `--no-deinterlace`,
`--no-progressive`, `--no-anti-blur`, `--no-adapt-width`, `--no-whole-frame`, `--no-seamless-scroll` and `--ps2-scanout` keep their switches off
(and `--flicker-filter` and `--ps2-scanout` the flicker filter on),
`--widescreen` keeps widescreen on, `--frame-rate` sets the frame rate, `--draw-distance` and `--draw-distance-sky` set those, `--myus-necklace` and `--separate-run` keep
those mods on, and `--controls` picks the layout.

**Tests see none of it.** A headless run or one that plays a pad script (`--input`) reads no
settings.json and has no overlay, so `boot_test.py` and `frame_test.py` run the same whatever the
player has chosen. `frame_test.py` and `map_tour.py` ask for `--renderer gs` (paraLLEl-GS, the
reference), whatever the program's default; `--renderer native` runs them on the native GS. Built
without SDL there is no overlay at all.

`KF4_OVERLAY_SHOT=page@vblank,...` is for checking the overlay without anyone at the screen: it opens
the overlay on that page at that vblank, even with a pad script (in a hidden window), and writes the
window's whole picture a few vblanks later as `overlay_<page>.png` in `--dump-dir`. The pages are
`display`, `audio`, `controls`, `savestates`, `mods`, `debug` and `interface`, and `closed` is the game alone;
`map@V` and `mini@V` (the automap mod), `menu@V` (the game alone, for the extended stats mod's lines in
its menus) and `profiler@V` (the profiler's panel, Shift+P, shown from the first) are named by their vblank,
`overlay_menu_V.png`, so several can be taken in one run. List them
in vblank order. Use it with `KF4_DATA_DIR`, as it reads and, after the last picture,
writes back settings.json:

```sh
KF4_DATA_DIR=/tmp/kf4-data KF4_OVERLAY_SHOT=display@1990,debug@2000 \
  out/windows-clang/kf4 --fast --frames 2010 --input tests/newgame.pad --dump-dir /tmp/shots
```

A pad script has one press per line: `<vblank> <buttons> [vblanks held, default 6]`. The buttons are
names such as `CROSS`, `START`, `UP`, joined with `+`, or `-` for none. A line `<vblank> lx=N ...` (or `ly`, `rx`,
`ry`) sets a stick (0x80 is the middle). Text from `#` on is a comment. `tests/newgame.pad` boots to
a new game.

The log has one line per vblank event (`<vblank> event: ...`: files read, movies, the memory card),
and one per game frame with the game's status, the map and the player's position.

`tools/boot_test.py --disc kfiv_us.iso` is step 1's check: it boots to a new game twice and checks
the two logs match and reach the first map. It runs headless with no GS, so it needs no GPU.

`tools/frame_test.py --disc kfiv_us.iso` is step 2's: the same boot with the GS, dumping the logo,
the title menu, the brightness screen, the opening and the first map, and every picture of vblanks 990
to 1010 (`--dense`). It checks two runs (`--runs N` for more) give the same pictures byte for byte and
that none is blank (`--keep DIR` keeps them to look at), and that a third run dumping only the last
picture gets it byte for byte as well: reading pictures back must leave nothing behind in the GS. It
draws with paraLLEl-GS; `--renderer native` makes the same checks of the native GS.

Each folder of the game code is its own target, so `cmake --build out/windows-clang --target game_ui`
compiles just the `ui` folder (`src/game/ui`, with the patches applied).

## Mods

Optional changes to how the game plays. All are off by default; with them off, the game plays as it
does on the PS2. Each is a switch on the command line and on the overlay's Mods page (live), kept in `settings.json`'s `"mods"`, and read by the
game's patches through a function of `port/include/port/host.h`; off, the game code does exactly what
it does without it.

- **Myu's Necklace effect without wearing it** (`--myus-necklace`, `port_myus_necklace()`).
  `MapObject_DrawAll` decides each frame whether the necklace is worn, from the amulet slot; with the
  mod on it counts as worn whatever the slot holds, so the objects it makes glow always glow (their
  pulse starts when the mod is turned on), and the slot is free for another amulet.
- **Separate run key** (`--separate-run`, `port_controls_run_held()`). The game runs while its
  action button (cross by default) is held with a move, and uses what is in front when it is
  pressed. With the mod on, `Player_Update` has the action button's held bit be the Run control's
  (Left Shift and a controller's left stick press in both layouts, rebindable on the Controls page,
  shown there only with the mod on), so the action button only uses things. Pressed buttons are
  untouched, and everything that stops running (weight, item use, death) still does. A pad script
  (`--input`, `--record-input`) keeps the pad as it is.
- **Extended stats** (`--extended-stats`, `port_stats_options`; [EXTENDED_STATS.md](EXTENDED_STATS.md)).
  What the game keeps and never shows, written into the empty places of its own menus whenever they
  are up; it only reads the game, so on or off the game runs the same. The main menu and Status: the
  experience to the next level, the weapon hits and spells to the next Physical and Magical point
  (every 100th that deals damage), the hits to the weapon's next level (180 and 480), and the ailments'
  time left. Equipment: the weapon's level, the ailments and the defences against them, each piece's
  durability (the percent its values count at, 100 down to 50), and the hidden effects of what is worn.
  Item and Arrange: what the item under the cursor does (the HP and MP it restores, the ailments it
  cures, the spell a crystal teaches, ...), read from the game's item tables. The free-floating panel
  (F6, rebindable: "Show / hide the stats panel (mod)") has all of it and more (spell levels, gauges);
  the hit log (bottom left) lists each hit dealt and taken with the HP left, and the creature last hit
  gets a health bar (top). Its parts are switches of their own on the Mods page (`settings.json`
  `"mods"`: `extended_stats`, `extended_stats_menus`, `extended_stats_panel`, `extended_stats_damage`,
  `extended_stats_durability`).
- **Automap** (`--automap`, the Mods page's "Map" section; [AUTOMAP.md](AUTOMAP.md)). A map of the map you
  are in, made from its collision meshes, that fills in as you explore and is kept with each save (quick
  saves and memory card saves; a new game starts blank), in the look of the KF3 port's automap: floors
  shaded by height, walls, water, lava, poison and deadly drops, the ways into other maps, doors (locked or
  not), chests (opened or not), barrels and bones, save points and people, named from the game's own text.
  M (the Controls page's Map) opens the full map, which holds the game (wheel: zoom, drag: move, hover: a
  label); a minimap shows in a corner while playing. Its cheats, off by default, stand in for a guide:
  the whole map, items and what containers hold, live enemies, secrets and traps, and what each lock needs
  with hand-written hints (`game/automap_guide.c`). Only reads the game; with it off nothing of it runs.
- **Weapon sets** (`--weapon-sets`, the Mods page; [WEAPON_SETS.md](WEAPON_SETS.md)). A second weapon and shield,
  swapped with the ones in hand on one key (R, a controller's right stick press; rebindable: "Swap weapon sets"),
  through the game's own equipping (the stats, the held weapon's model, the gauge). Chosen on the equipment screen:
  Up from Weapon reaches Shield 2 and Weapon 2, where Cross keeps the item for set 2. Ignored mid-swing and
  wherever the game would not open its menu. Set 2 is kept with each save, as the automap's maps are.

## Widescreen

Off by default, so the picture is the PS2's. `--widescreen`, or the overlay's Display page (live),
turns it on:
- **The 3D view is 16:9.** The game's projection (`GameLoop_SetCameraMatrices`, and the King's Map's
  in `MapViewer_RunKingsMap`) is three quarters as wide, so the GS still draws 640x448, now holding a
  16:9 view at the same height (vertical field of view unchanged), squeezed. The window shows it at
  16:9 (and a window that is not full screen is made 16:9). Nothing pops in at the sides: the game
  culls its map records with a half-plane in front of the player, and everything else by where it
  lands on the screen, which the wider projection takes care of.
- **The 2D keeps its 4:3 shape.** The GS front end (`port/platform/gs.c`) squeezes it to three quarters
  of its width as the packets go by: everything drawn by PATH3 (the menus with their item models, the
  title, the memory card notice), and the 2D of the packet chain that the game's patches mark with a
  write to `PORT_GS_REG_WIDESCREEN` (`game/widescreen.c`): the message text, the pause label, the
  splash pictures, the map viewers' pictures, and the HUD, which goes into the 16:9 picture's top-left
  corner or (`--widescreen-hud-centred`, or the overlay) stays where it is at 4:3.
- **Left full width:** pictures of the frame itself (the dimmed world behind the menus, the screen
  wobble, the frame copied at TBP 0x1180 or 0x2BC0, every screen effect that redraws the frame),
  full-screen fades and flashes, and untextured panels that reach the screen's edge.
- **`--dump-frames` writes the GS's picture as it is**: 640x448, squeezed. Stretch it to 16:9 (for
  example to 796x448) to see it as the window shows it. `KF4_OVERLAY_SHOT` writes the window, so its
  pictures are 16:9.
- Off, the game code does exactly what it does without the switch (`port_widescreen()` is 0, nothing is
  marked, and the GS front end does not touch the packets): the frame and boot tests, `--stats`' VU1
  counts and PATH1 hash are main's.
- Not done: the 2D is squeezed into 480 of the 640 columns, so it is a little softer than at 4:3 (a
  higher internal resolution, step 3, would mend that). Anything the game sizes in screen pixels around
  a projected point would come out a third wider; none has been seen yet.

## Draw distance

Off by default (1x, the PS2's fog). `--draw-distance 1.5|2|4|max`, or the overlay's Display page (live),
lets the player see further. What ends the view in this game is the fog, not a cut: the game draws every
map record the visibility table of the record the player stands on lists (`func_00112BD0`: a half-plane
behind the player and that table; the map header's draw distance at 0x7C would cut by distance, but it is
0 in all 64 maps), with the objects, entities and items on them, and no distance cut of their own; the
far plane (`GameLoop_SetCameraMatrices`, Z 0 at about 70,000) lies beyond everything the fog leaves.
- **The fog goes out N times as far** (`game/draw_distance.c`). A fog block is `near + far / w` (the GS's
  fog value, w the distance), so the same fog N times as far only scales the far term. Each map's blocks
  are copied into `map_draw_state` every frame, and every model points into that copy, so the copy is
  scaled; the current block eases to the new value over 30 frames, so a change in play fades in.
  Blocks of constant fog (a tint, far 0) stay as they are. Max scales by 1024: no distance fog, also not
  the mist that is part of a place's look (the Holy Forest, map 013, loses its haze).
- **The far plane** moves out with it (at most 16 times, from where Z never reaches 0): with the fog kept,
  moving the far plane alone changes nothing that shows (map 000's long view: 65 pixels of 6.9M, Z
  rounding), so there is no switch for it alone.
- **The sky keeps the PS2's fog** (`--draw-distance-sky ps2`, the default): the records that follow the
  player (flags bit 31: the sky dome on maps 000 and 013, backdrops on 002, 004, 021, 027, 098 and 099)
  are a backdrop at a fixed distance, so pushing their fog out shows nothing further away, it only
  lightens the sky (map 000's dark sky shows its clouds at 2x). `push` (or the overlay) does it anyway.
- **What it shows:** on the outdoor maps the far walls, ruins and the lava mountains of map 000, the end
  of long corridors (021), the back of caves. **Where the game draws nothing** (a record the visibility
  table leaves out, behind a turn the PS2 never shows, or the neighbouring map not streamed in yet) the
  background is the fog's colour, as before; with the fog further out such an edge can show as a straight
  line into the fog's colour (map 016's halls: black, so it reads as a dark doorway). The game's
  visibility tables and its two map slots are not changed.
- **Gameplay is the PS2's**: only fog values and the projection's Z change; which records, objects and
  entities are drawn, wake and act is not (the game's log of a new game is byte for byte the same at
  max). The cost is nothing measurable: the same draws and VU1 work (2,100 vblanks at `--native-scale 4`,
  MSAA 2, 60 a second on maps 000, 009 and 013: the same VU1 runs and instructions, the times within the
  runs' noise).
- Off, the game code does exactly what it did (the fog blocks and the far plane untouched): the pictures
  and `--stats`' PATH1 hash are the build's before, byte for byte.

## Higher frame rates

Off by default (30 pictures a second in play, as on the PS2). `--frame-rate 60|120|144|240|N|monitor`,
or the overlay's Display page (live; `settings.json` `display.frame_rate`, a number or `"monitor"`),
turns it on; `monitor` is the refresh rate of the window's screen. The game itself still runs at 30
frames a second: in play it draws a frame every second vblank and shows it for two, and what it does
is the PS2's, input for input. With the switch on, the pictures between two of its frames are drawn
by the PC from the frame it has just drawn (shown at the next vblank), everything in it moved to where
it is at that picture's time, t of the way from the frame shown to the one drawn
(`port/platform/frame_interp.c`; STEP3.md, "Higher frame rates"):
- **The camera** (turning and walking): the rotation by slerp, the place along the line.
- **Models and effects**: each model part is paired with itself in the frame before (by its batch, or,
  where the CPU makes it anew each frame, by its order and what it is drawn with) and drawn t of the
  way between its two places: the enemies, the arm's swing, a spell's sparks, moving objects.
- **The HUD and the 2D**: each item the CPU draws (a sprite, a strip) is paired with the frame
  before's in order and drawn with its places, colours and texture coordinates t of the way: a gauge
  growing, a fade, the brightness passes, the scrolling textures' copies (the sky, the water's ripple
  strips).
- **The reflection** (the water on map 009) moves with the camera too, and the previous frame blended
  in (`Game_BlendPreviousFrame`, while aiming) blends the frame shown into every picture between, so
  the trail is as long and as soft as the game's.
- **Not interpolated**, as no interpolation can make them: pictures the game makes anew (the HUD
  orb's swirl, the fire's flames and the other flipbook animations change texture, not place: they
  change once a frame, at the first picture after it), a model that comes or goes, things two frames
  apart that the pairing cannot tell apart (two look-alike models crossing), a cut (a teleport, a map
  change, a scripted jump of the camera). A fade steps with the GS's 8-bit alpha: at 240, a step every
  second picture.
- **No input lag is added.** The pictures between show the camera's next move early; the game's own
  frame is shown at its vblank as before.
- **The window.** At 60 every vblank has its picture. Above 60 the pictures are timed: n a vblank when
  the rate is n times 59.94 (120, 240), else at even steps wherever they fall (144, 165). With the
  Vulkan presenter a thread of its own shows each a quarter of a vblank after its time (so each is
  there before it is due); with SDL_Renderer the game's thread shows them while it waits for the next
  vblank and from its kernel calls. With vsync, choose the screen's rate (or `monitor`). If the renderer
  cannot keep up, pictures between are left out rather than the game held up (`--stats` counts them).
- **The menus, the title and the movies** are as they were: they draw no world. paraLLEl-GS
  (`--renderer gs`) and `--no-whole-frame` stay at one picture a vblank (60).
- **The cost** is the GS's: each picture between draws the frame again (VU1 is not run again, and
  the frame's uploads are asked for again from the native GS rather than sent). About 1.1 to 1.5 ms
  a picture on a Ryzen 9 9900X3D with an RTX 5080: 2,000 vblanks headless with `--fast`, walking on
  map 000, take 3.0 s at 30, 4.2 s at 60, 5.9 s at 120, 7.8 s at 144 and 9.7 s at 240 (map 009: 2.8,
  3.8, 5.0, 6.2, 7.6 s). In real time 240 is shown at 240 a second on map 009 and at 228 walking up to
  the gate of map 000, the heaviest place tried.
- Off, nothing changes: the game queues no marks and the GS front end keeps nothing. On, the game's
  own frames (the even vblanks) are still the same pixels, byte for byte, at every rate; only the
  pictures between are new. `--dump-frames V` also writes the pictures between of vblank V, as
  `frame_V_PPP.png` (PPP: thousandths of the vblank), the same every run. `KF4_INTERP_STILL=1`
  draws the pictures between with the frame's own camera (a check of the replay);
  `KF4_INTERP_DRAW_ALL=1` draws every picture between headless (for speed tests); `KF4_PRESENT_SIM=HZ`
  runs the window's pacing headless with nothing drawn (`KF4_PRESENT_LOG=FILE` writes each present).
  `KF4_INTERP_OUTSIDE=1` logs each run of the PS2 path's world (frame_interp.c's runs; not native
  draws) whose place on the screen in a picture between leaves the union of its places at t 0 and t 1
  (the vblank, phase, run, pair and the three boxes; `=2` also every shadow run, program 0x5A0;
  `KF4_INTERP_OUTSIDE_LOG=FILE` for a file). `KF4_GSN_SLOW_US=N` holds the native GS's thread N us at
  each vblank, so that a window falls behind as on a busy GPU (only the vblanks' own pictures shown).
  `KF4_FX=water,drain,prev,...` forces the game's full-screen effects on for the draw only
  (`game/screen_fx.c`; the list and what each is in docs/port/SCREEN_EFFECTS.md), for
  `tools/screen_fx_test.py`.
- `--gs-capture-at` and F9 record the GS's packets as the game sent them, without the pictures
  between.

## Saving: quick saves, save states and the memory card

There are three ways to keep a game, for different jobs:

| | Quick saves (F2, F4) | Save states (F5, F7) | Memory card saves |
|---|---|---|---|
| For | playing | debugging and tests | playing, as on the PS2 |
| Taken | anywhere in play (not in menus, messages or events) | at any vblank | at a save point, through the game's menus |
| Loads in | any build of the program | only the build that made it | any build, and on a PS2 or PCSX2 |
| Holds | the game's own save, where you stand | everything, exactly | the game's own save |
| Where | `quicksaves/quick<N>.kf4quick` in the data folder | `states/slot<N>.kf4state` | `memcard/` in the data folder |

Use quick saves to play: the port is rebuilt after each fix, and a quick save survives that, as a save
state does not. A memory card save is still the one to keep for good (it is what the game itself made,
and it loads on a PS2 too).

## Quick saves

A quick save is the game's own save, taken where the player stands, without a save point and without
the memory card. F2 takes one into the slot chosen on the overlay's Save states page (0 at first) and F4
loads it; F4 also works on the title screen. Both only act in play: in a menu, a message, an event, a
map change or a fade, a key's request is refused with a message (the overlay shows it a moment).
F2 is also refused while a script the game set off in play is still running (a door's, a lever's, a
death's: "an event is still under way (map script N)"): the save image holds what the script has done
so far but not the script, so the rest of it would never happen after a load. The Dark Knight's door on
map 036 is one: it closes behind the player and shows the text that sets the knight on, and a save taken
while it stood open loaded with the door open, no text, and the knight standing still for good
(`Script_EventRunning`, script/script.c). Such scripts end soon: a door's once the player has walked on.
`--quick-save-at` waits for them.

How it works (`game/quicksave.c`, `port/platform/quicksave.c`):
- **Saving** calls the game's own writer, `Game_WriteSaveImage` (`core/game.c`), as the save screen
  does before a memory card save (`ui/trade.c`, after `PlayTime_Update`), and writes the image to a file
  instead of the card. The patched main loop asks for it once a frame, at its end (`GameLoop_Run`,
  `Quick_PollGame`), so it is taken between two frames. `save_image` is put back as it was. The image is
  the memory card's, byte for byte: the map (`saved_map_tag`), the player's position and rotation
  (heading and look), HP and MP, level, experience, gold, stats, what is worn and held, the items, magic
  and their levels, key items, the play time, the game's options and button layout, and the world's
  flags (doors and chests, map objects, items picked up and dropped, events, story values). Saving also
  stores the map objects' flags first (`MapObject_SaveAllFlags`), as any save does.
- **Loading** puts the image where a memory card load puts it (`save/save_card.c`: `save_loaded = 1`,
  `game_status.bit9`), after checking it as the game does (`Game_CheckSaveImage`). In play that ends the
  main loop as a card load does, and `main` restarts the session from it: `GameLoop_ResetSystems` (which
  reads the image back with `Game_ReadSaveImage`), then `GameLoop_Run` on the saved map, which puts the
  player where the image says. A player who is dying is saved from it (the game over is not shown);
  once the game over screen is up, go back to the title to load. On
  the title screen, `Title_Run` returns as the title's own Load does after a load (`TitleMenu_Run`'s
  case 5). The first vblank of play then checks where the player is against the file, and the log says
  so (`quick loaded: ... (as saved)`).
- **The neighbouring map.** The game keeps two maps: the one the player is in and the one next to it,
  which it streams in as he walks (`MapLoad_Update`, `map/map_load.c`). A map record that names one of
  the map's origins (`neighbour_origin_index`) asks for that neighbour, which is then read in the
  background; a record at the join (`needs_neighbour_map`) has it set up, so that it is drawn and walked
  on. The records in between name nothing and keep whatever was loaded last, so what is loaded depends
  on the way the player came, which the save image does not hold (a card save is taken at a save point,
  where it does not matter). A quick save keeps it in its PC block: which origin led to the neighbour,
  and whether it was set up or only read (or being read). Loading asks the game's own loader for it
  once the saved map is set up (`Quick_RestoreNeighbour`, called from `GameLoop_Run`) and runs its steps
  as far as they had gone, before the first picture. A quick save from before this was kept (a 64-byte
  PC block) is given the neighbour named by the record the player stands in (read only, as the game has
  it there), or else by the nearest record that names one (set up). The log says which
  (`quick load: neighbouring map 08 (origin 3) set up`).
- **The file** is a 40-byte header (`KF4QUICK`, the format's version, the save image's version word
  0x15400, sizes, a hash), a PC block (when, the map, position, rotation, HP, MP, level, gold, play
  time: what the overlay lists; then the neighbouring map) and the save image. The PC block's size is
  the format's minor version: a reader takes what it knows and skips the rest, so older and newer
  files of format version 1 load in every build. Every number is written byte by byte, little-endian,
  so nothing in it depends on the build. A file that is not a quick save, is of another format version,
  is cut short, fails its hash or fails the game's own check is refused with a message, and the game goes
  on as it was. Nothing is ever read from or written to the memory card.
- **Made-up files.** `tools/map_tour.py` writes quick saves for any map and place itself (from a real
  one, with the map and position changed) and marks them with bit 1 of the PC block's flags byte. Such
  a file loads as any other; then the player is put on the floor nearest under the position (or else
  over it: `Collision_FindFloorBelow`) and the neighbouring map is worked out from there. The game's own
  quick saves never have the bit. A teleport (below) is loaded the same way.

### Teleport

A debugging aid on the overlay's Debug page (and `--teleport`, for tests): pick a map and a place in it,
and the player is put there, with the game's progress as it is now. It is a quick load of a save image
made from the game this moment, with the map and the place changed (`port/platform/teleport.c`,
`game/quicksave.c`): the game restarts the session on that map as a memory card load does, the player
is put on the floor under the place, and the neighbouring map is worked out from there. Like a quick
load it is taken in play. The places are those `tools/map_tour.py` visits, read from the map's file on
the disc: the map's start (where a map change without a link puts the player), the map's own items
that lie on a floor, then the middles of the biggest floors. A map the story has not reached yet may
not behave as it does in a real playthrough (its doors, events and enemies follow the world's flags).

**Map objects.** The maps being left (both slots) file their objects' flags (doors, chests, switches,
levers) under their own numbers first, as leaving a map does, and the map teleported to starts from
its own saved flags, as after a map change. Before 2026-10-25 the map being left's flags were filed
under the new map's number: the new map started with the old one's states (in a save, the new map's
flags are then the old one's), and the old one lost what changed since it was entered (a map visited
for the first time and left by teleport, with no save between, kept blank flags, which it started from
when entered again). Such flags look like real ones, so a save cannot be checked or mended for it.

**Dead ends.** 19 maps never ask for a neighbouring map themselves (their records name none of their
links: 009, 011, 014, 021, 025, 030, 034, 036, 038, 040, 041, 042, 043, 044, 097, 106, 110, 114, 116).
The game enters each from the one map whose link leads to it, and that map stays loaded in the other
slot: 040's entrance looks back into 039, and walking out of it walks onto 039's floor. A quick load
in such a map (a teleport, or a quick save taken there) finds no neighbour of the map's own, so it
loads that map, set up, where its link puts it (`port_map_way_in`, read from the map files); without
it the way out is black and the player falls through the world there.

What a quick load restores is what loading a memory card save restores, at the place it was taken:
- **Restored:** the map, the exact position, heading and look; HP and MP; everything else in the
  save image (above); and the neighbouring map, read or set up as it was (above): a quick save taken
  just outside a cave's entrance, or just inside it, loads with the other side there.
- **Not restored, as after any load of a save:** the map is entered afresh, so enemies are back as when
  you enter a map, and map objects that move (lifts, platforms) are where the map starts them (doors,
  chests and switches keep their flags). The neighbouring map is entered afresh in the same way. What
  lasts a while starts afresh: the power and magic gauges, the timers of ailments and of magic and items
  in effect, the Power Seed and the Widda Needle. A map's own counters that count its loads move on as
  on any load. The record the player stands in is found again from the position, as on a card load (it
  is found from the floor once he moves).
- A quick save taken on a moving platform or in a hazard puts you back on the same spot, where the
  platform may not be (you fall) or the hazard hurts you again.

For tests, `--quick-save-at 1900:1` takes a quick save into slot 1 at vblank 1900 (it waits while the
player is not in control), and `--quick-load 1` loads slot 1 on the title screen (`--quick-load
2100:1` in play, once the run reaches vblank 2100). Checked on Windows with `tests/newgame.pad` and a
walk:
- a quick save in the first map, away from the start, loaded in a new process from the title and in
  play: the player is back on map 0x63 at the same position, heading and look (`(as saved)`), and the
  picture is the same view;
- a quick save taken again right after that load has the same save image but the play time and one map
  script counter;
- loaded in a build with an unrelated code change: the same (that build refuses the save state taken at
  the same vblank);
- loaded while the player burns to death (HP falling, and after it reached 0, before the game over
  screen): he is back where it was saved, with its HP;
- a damaged, cut short, other-version or foreign file is refused, and the game goes on;
- the memory card folder is the same before and after;
- the neighbouring map, from a quick save taken outside the poison cave's entrance (map 0x63, the cave
  is map 08): through the entrance the cave is there (it was dark, and the player fell through it,
  before); saves taken on the way in on both sides of the join (map 0x63 with 08 set up, map 08 with
  0x63 set up, looking back out shows the outside), at the new game's start on a record that asks for
  map 00 and while that load was running (restored read only, as the game had it), all restore what
  the game had; each of them with its PC block cut to the old 64 bytes works out the same neighbour.

## Save states

A save state is the whole running game in a file: loaded, the game goes on exactly where it was.
They are for testing and debugging (to play, use quick saves, above). The settings
overlay's keys (F5, F7) save and load slot 0; the command line drives them for tests:
- `--state-save-at 900:1` saves slot 1 at vblank 900; `--state-save-at 400:a.kf4state,2000:b.kf4state`
  saves two files in one run.
- `--state-load 1` loads slot 1 before the game's first instruction; `--state-load 300:b.kf4state` loads
  a file once the run reaches vblank 300, while it runs (in a window too).

Slots are `slot<N>.kf4state` in `%APPDATA%\holyland\states` on Windows, and in
`$XDG_DATA_HOME/holyland/states` (or `~/.local/share/holyland/states`) elsewhere; with `KF4_DATA_DIR` set, in its `states` folder, next to settings.json. A file is about 5 to
15 MB (LZ4-compressed; 80 MB in memory).

How it works (`port/platform/savestate.c`):
- A state is taken at a vblank, where every EE thread is parked in a kernel call. It is not taken
  while a GIF packet or an image transfer to the GS is half sent, or a `host0:` file is open: the
  request then waits for the next vblank, and says so.
- It holds:
  - game RAM, the scratchpad, VU0 and the register pages;
  - the globals of the game and of ico-audio's SPU2, which are linked as one block between two
    markers (`port/platform/state_begin.c`, `state_end.c`);
  - the EE threads' fibers and their stacks, and the kernel, DMA controller, VIF1, VU1 and GIF;
  - the GS's VRAM and registers, and what paraLLEl-GS's scanout keeps from the last pictures;
  - the libraries, the IOP's memory, libsd, FSIOPSND, modmidi, modhsyn and the sound's clock.
- Not in a state: what is the host's (the window, the sound device, the log, files, the options) and
  `--stats`' counts.

**Same build only.** The stacks hold return addresses into the program, and the game keeps pointers
in game RAM and its globals. So:
- game RAM and the stacks are at fixed addresses;
- the program is linked without ASLR;
- a state carries a hash of the program's code, and another build refuses it.

A state loads into a run with other options:
- On the device's clock or the game's: the sound goes on from where it was.
- Without the GS (`--headless` with no `--dump-frames` or capture, or `--no-gs`): a state taken without it has no
  VRAM, and one loaded without it skips its VRAM.

The same input gives the same run after a load as without one. Checked on `tests/newgame.pad`, with
states at vblanks 400, 900 and 2000, the GS and the sound on, to vblank 2300. A run that saves
against one that loads the state in a new process:
- the per-frame log after the "state saved" line is the same as after the "state loaded" line;
- the `--audio-dump` WAV is the same from the state's SPU2 frame on;
- `--stats`' VU1 runs, instructions and cycles and the PATH1 hash are the same;
- the frames dumped after the state's vblank are the same, byte for byte;
- the run that saved is the same as one that does not (log, WAV, frames);
- a state saved again right after it is loaded is the same file (but the time in its header).

## Debugging

Tools for finding what is wrong while playing, in every build: they cost nothing until used, and
change nothing the game does. Each writes a file into `captures` in the data folder (beside
settings.json; `KF4_DATA_DIR` moves it), and the overlay's Debug page has a button for each, the
folder and the last file written. The files are read by any build: a save state loads only in the
build that saved it, these do not.

| Key | What it writes |
|---|---|
| F9 | `gs_<vblank>_<time>.kf4gs`: a GS frame capture, for `kf4-gsreplay` (below) |
| F10 | `state_<vblank>_<time>.json`: the game's state for a bug report: the map, the player (position, heading, HP, MP, stats, equipment, ailments and their timers, status bits), the game's status, the live entities (type, spawn, position, HP, AI state, flags), the map objects within 6000 units (kind, state, position), the story's flags that are set (story, visited maps, map script flags, recollections) and the port's last 32 events (files read, the memory card) |
| F12 | `audio_<vblank>_<time>.wav`: the last 20 seconds of the sound, as the game made it (before the volume) |
| F8 | `flash_<vblank>_<time>/`: the flash recorder's folder: the last 2 seconds of pictures shown (below), a GS capture of the next two vblanks, a quick save and a save state |

**The flash recorder (F8)** is for a picture that is wrong for a moment, a flash that a screenshot or
F9 misses because it is gone by the time the key is pressed. The Vulkan presenter keeps every picture it
shows, scaled to 540 rows (960 x 540 in 16:9), in a ring on the GPU: the last 2 seconds, 480 pictures at
240 (about 1.1 GB of GPU memory at 240; 60 pictures a second at least). Press F8 right after seeing the
flash: the folder `captures/flash_<vblank>_<time>` gets the ring's pictures, oldest first, as
`0000.png`, `0001.png` ... (`pictures.txt` says each one's vblank, its phase, 0 for the vblank's own
picture and above for a picture between, and how long before the last it was shown), with
`gs_<vblank>.kf4gs` (the next two vblanks, as F9), `quick.kf4quick` (load it with F4's slot or
`--quick-load`, any build) and `state.kf4state` (`--state-load`, this build). The pictures are written on a
thread of their own in a few seconds (about 700 MB at 540 rows: uncompressed PNGs); the ring takes no
pictures meanwhile, and the toast says "recorded N pictures" when they are all there. Then

```sh
python tools/flash_find.py captures/flash_001500_20261023-210057
```

lists the pictures that differ from both their neighbours (a flash, also one that lasts all the pictures
between of a game frame) and writes `flashes.png`, a contact sheet of each with the pictures before and
after it and where it differs (it reads `--dump-frames` folders too: `frame_000436_250.png`...). The cost
while playing is one small copy a picture: 5.4 us of GPU time at 240 at 4x (0.13% of the GPU), the
same pictures a second as without it. `--flash-recorder 1,360` keeps 1 second at 360 rows, `off` none;
the Debug page turns it on and off for the run. Only the Vulkan presenter has it (the default with the
native GS); with SDL_Renderer the key writes the GS capture and the saves alone.

**A GS frame capture** holds, at the start of a frame, the GS (its 4 MB of VRAM, its registers and
the scanout's last fields), VU1 (micro and data memory, registers and flags) and VIF1, then
everything that reaches them until the frame ends, in order: every GIF packet with its path, every
VIF1 word DMA channel 1 sent, each VU1 program started, the read-backs and the two vsyncs (both fields
of the game's 30 Hz frame). And the vblank, the map and the player's position (the log's line), the
build, and the picture options. It is taken right after a scanout, and waits a vblank if a packet
is half sent. It is LZ4-compressed: 3 to 10 MB. `--gs-capture-at 2290` captures vblanks 2290 and
2291 (`2290:4` four of them), the same pictures `--dump-frames 2290,2291` writes.
`port/include/port/gs_capture_format.h` is the format.

**kf4-gsreplay** (`out/<preset>/kf4-gsreplay`, built with paraLLEl-GS) draws a capture again with
paraLLEl-GS, either from the GIF packets as recorded (`--mode gif`, the default) or from the VIF1
stream run again through the VU1 interpreter (`--mode vif`), which makes PATH1 and PATH2 again and
compares each packet with the recorded one. Both give the pictures `--dump-frames` gave, byte for
byte. `tools/README.md` has its options and examples:

```sh
kf4-gsreplay gs.kf4gs --info                     # what and where: vblank, map, position, build
kf4-gsreplay gs.kf4gs --frames --out pics        # the scanouts again: frame_NNNNNN.png
kf4-gsreplay gs.kf4gs --list > draws.txt         # every draw: path, VU1 program, PRIM, box, Z range, GS state
kf4-gsreplay gs.kf4gs --draws 400-420 --zbuf --out pics   # the frame buffer (and Z buffer) after each draw
kf4-gsreplay gs.kf4gs --pixel 320,200            # the draws that cover a pixel, and its colour and Z after each
kf4-gsreplay gs.kf4gs --vertices 412             # a draw's vertices
kf4-gsreplay gs.kf4gs --mode vif --vu-round nearest   # VU floats rounded to nearest: which vertices move
kf4-gsreplay a.kf4gs --compare b.kf4gs           # two captures, draw by draw
```

With `--renderer native` it draws with the native GS instead, and with `--renderer both` with both
(`--draws` and `--frames` then write `_native` pictures beside paraLLEl-GS's). `--renderer modern` is
the native GS with the scene in its own targets (P8, [NATIVE_DRAW_P8.md](NATIVE_DRAW_P8.md)), from
the capture's start; a capture taken with the game's `--renderer modern` carries the page-ownership
table (record `SA_TABLE`), so the scene's textures come from their uploads from the first draw, and
the `[stats] standalone` line at the end says where each came from. A save state taken in modern
carries the table too.
`--compare-renderers` runs both and, after each draw, compares the pixels of its box: it lists the
draws whose box differs by more than 1 in a channel, then puts the native GS back in step with
paraLLEl-GS's pixels so that the next draw listed made its own difference (`--no-resync`: not). At
each vblank, and once more at the end of the capture (a capture's last frame is drawn after its
last vblank), it compares the frame buffer the display shows (`--frames`: `vblank_N_gs.png`,
`end_N_native.png`...), and at each vblank the two scanouts (the pictures shown: a `scanout N` line,
and a `scanouts:` total at the end). `--draws` limits the per-draw comparison to a range (`--draws
999999-999999`: only the vblanks and the end). paraLLEl-GS's pixels are read back through the GS (a local-to-host
transfer); for some field-mode menu buffers that read is stale, and the scanout pictures
(`--frames --renderer both`) are the ones to trust.

```sh
kf4-gsreplay gs.kf4gs --compare-renderers --limit 50           # the draws the native GS draws differently
kf4-gsreplay gs.kf4gs --compare-renderers --draws 3050-3060 --out pics   # and their pictures
```

Without a GPU, `--info`, `--list`, `--vertices` and `--compare` still work. A draw is a run of vertices
with no GS register written between them (other than the vertex's own colour and texture
coordinates), within one GIF packet; pixel coordinates are the frame buffer's (as `--draws` writes
it). Limits: the GS's CLUT buffer is on the chip and not in a capture, so the first CLUT a capture's
draws compare against (TEX0's CLD 4 or 5) is loaded again from VRAM; with `--widescreen` on,
`--mode vif` makes the packets as the game sent them, before the GS front end squeezed the 2D, so
those differ from the recorded ones.

`KF4_DEBUG_AT=state@2290,audio@2290,gs@2295,flash@2300` presses F10, F12, F9 and F8 at those vblanks, for checking the
tools without anyone at the keyboard (in a run that draws; with `KF4_DATA_DIR`, as they write into its
`captures` folder).

## Sanitizers

`-DKF4_SANITIZE=address,undefined` (any `-fsanitize=` list) builds everything with AddressSanitizer
and UBSan; left empty, the build is as it was. Use it on Linux with gcc, without the GS and SDL:

```sh
cmake -S . -B out/asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=gcc \
  -DKF4_USE_PARALLEL_GS=OFF -DKF4_USE_SDL=OFF -DKF4_SANITIZE=address,undefined
cmake --build out/asan
ASAN_OPTIONS=halt_on_error=0 out/asan/kf4 --disc kfiv_us.iso --fast --headless --frames 3000 \
  --memcard none --input tests/newgame.pad --log run.log 2> run.err
```

- gcc needs its sanitizer libraries (Fedora: `libasan`, `libubsan`, `libtsan`); without them the build stops
  at the first link. clang's come with it: `cmake --preset linux-clang -B out/linux-clang-asan
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKF4_SANITIZE=address,undefined -DKF4_TESTS=ON` (or `thread` for TSan,
  whose reports in the game's DMA and packet code are between fibers it does not know of).
- The checks are recoverable: `ASAN_OPTIONS=halt_on_error=0` lists every report of a run instead of
  stopping at the first. UBSan goes on by itself.
- The fibers tell ASan of each stack switch (`port/platform/fiber.c`, `port/include/port/sanitize.h`),
  and a fiber's stack is cleared of old poison when it is made or a save state puts it back.
- The program sets `detect_stack_use_after_return=0` (ASan's fake stacks would keep locals outside the
  fibers' stacks, where a save state does not see them) and `detect_leaks=0`; `ASAN_OPTIONS` overrides.
- Game RAM moves to 0x100400000000, above ASan's shadow memory (which covers the usual address), so save
  states still work. ASan does not check game RAM: it is one block of the program's, and the game's own
  allocator hands it out. Only the program's globals, the stacks and the C heap are checked.
- Save states copy the globals' image and the stacks with their redzones, without ASan's checks.
- Known reports, all harmless and as on the PS2: `title.c` reads `key_frames[4]` and `key_values[4]`
  (the next fields of the same `BootSpriteTrack`), and `movie.c` passes NULL to `memcpy` with a size
  of 0.

## Status

The steps, in order: [PLAN.md](PLAN.md).

**Step 0** (done): the game code compiles and links as a 64-bit program on Windows and Linux,
and `main` runs until it needs something the stand-ins do not do yet.

- `tools/port_census.py` counts, per folder, the errors and the inline asm still standing between
  each folder and that goal.
- `tools/port_census.py --show ui/menu_draw.c` prints one file's errors.
- `tools/port_census.py --show ui` prints a folder's.

**Step 1** (done): the stand-ins (thread scheduler, files, DMA, IOP, memory card, pad) get the game
from boot to a new game with nothing drawn, deterministically (`tools/boot_test.py`).

**Step 2** (done): the game is drawn, in a window, from boot to the first map, and played with a
pad or the keyboard. The DMA controller reads the VIF1 and GIF chains; VIF1 and an interpreter of
VU1 run the game's microprograms; the GS is paraLLEl-GS (`tools/frame_test.py`).

**Step 3** (done, October 2026): a renderer of our own. The native GS (`port/gs/native/`) draws on
Vulkan, the VU1 programs run as C (bit for bit the interpreter's), and the window is presented on
Vulkan; all three are the defaults, with paraLLEl-GS behind `--renderer gs` for comparison. Opt-in
on top: `--native-scale`, `--native-widescreen`, `--native-msaa`, `--frame-rate` (60 to 240).
[STEP3.md](STEP3.md) has the design and the progress; [RENDERER.md](RENDERER.md) is where to start
before changing anything that draws (the map of the code, the rules and the checks).

Sound (step 4) and movies (step 5) are in as well ([AUDIO.md](AUDIO.md)). What is left is in
STEP3.md's "What could come next" and PLAN.md.

## Layout

| Path | What |
|---|---|
| `decomp/` | submodule: kfiv-decomp at the pinned commit. Never edited here |
| `patches/` | the PC edits to `decomp/`, as diffs (`game/<folder>.patch`, `game/_top.patch`, `nonmatching.patch`) |
| `game/` | PC-only game code, compiled with the patched decomp (`game/data/`: the data sections in C; `state_log.c`: the per-frame log; `quicksave.c`: the game's side of quick saves) |
| `work/` | git-ignored: the decomp with the patches applied, for editing (`tools/patches.py apply`) |
| `port/include/` | the SDK headers the game includes (`libgraph.h`, `eekernel.h` …) for the PC, and `port/host.h` |
| `port/platform/` | the program's entry and options, game RAM, EE addresses, the register pages, the EE kernel (fibers, virtual time), the DMAC, the IPU's input DMA, VIF1, VU1, the GS's front end, the window and frame dumps, the disc, save states, quick saves |
| `port/gs/` | the GS backends behind one small C interface: `native/`, our own GS on Vulkan (the default; RENDERER.md), and `gs_parallel.cpp`, paraLLEl-GS |
| `port/sdk/` | the Sony libraries' PC versions: files and the IOP over SIF, the sound driver's RPC, libcdvd, libdma, libgraph, libpad (with `controls.c`, the keyboard, mouse and controller bindings), libmc, libsdr, libmpeg on the port's MPEG-2 decoder (`mpeg2.c`), and the PS2's math library (`libm_ps2.c`: newlib's, from the decomp's sources) |
| `port/overlay/` | the settings overlay (`overlay.cpp`, F1) and `settings.json` (`settings.c`) |
| `port/debug/` | the debugging tools: the GS frame capture (`gs_capture.c`, F9), the game's state as JSON (`game_dump.c`, F10), the sound's last 20 seconds, the flash recorder's folder and the captures folder (`debug.c`, F12, F8; the flash recorder's ring is `port/platform/present_vk.cpp`'s) |
| `port/math/` | libvu0 in C, computing what the SDK's VU0 code does, bit for bit (VU arithmetic from `port/include/port/vu_float.h`) |
| `third_party/parallel-gs/` | submodule: [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) (LGPL-3.0+), the GS in Vulkan compute |
| `third_party/imgui/`, `third_party/cjson/` | [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b with its SDL2 backends and [cJSON](https://github.com/DaveGamble/cJSON) 1.7.18 (both MIT), copied unchanged: see their `UPSTREAM.md` |
| `tools/` | `patches.py`, `port_census.py`, `port_abi_check.py`, `boot_test.py`, `frame_test.py`, `vu1_disasm.py` (the VU1 programs as text), `vif_replay_all.py` (VU1 replayed on many GS captures), `ee_libm_ref.py` (the retail program's math functions run on a model of the EE: `tests/libm_ref.txt`), `map_tour.py` (every map pictured, with a GS census), `gsreplay/` (`kf4-gsreplay`, which draws a GS frame capture again; `tools/README.md`) |
| `tests/` | pad scripts for headless runs (`newgame.pad`, `walk.pad`); `libm_test.c`, the PS2's math library against the retail program's results; `int_divide_test.c`, integer division as the EE does it; `vu1_test.c`, the VU1 interpreter's self-test, which also checks that VU0's arithmetic (`port/include/port/vu_float.h`) gives VU1's bits and that `port/math/libvu0.c` matches the SDK's VU0 code run as microcode (`out/<preset>/vu1_test`) |
| `.github/workflows/ci.yml` | CI: configure (the patches apply), build with paraLLEl-GS, `port_census.py` reports 0 errors |
| `CMakeLists.txt`, `CMakePresets.json` | the build; it compiles the patched game code from `out/<preset>/decomp_src/`, with `game/` |
| `docs/port/` | these notes |

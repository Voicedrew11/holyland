# tools

| Tool | What |
|---|---|
| `patches.py` | the PC edits to the decomp: `apply`, `save`, `upgrade` (docs/port/README.md) |
| `port_census.py` | per folder of the game code, the errors and inline asm left (docs/port/RULES.md) |
| `port_abi_check.py` | declarations that disagree with the definition |
| `boot_test.py`, `frame_test.py` | step 1's and step 2's checks: a headless boot to a new game, and its pictures |
| `state_test.py` | save states: one saved at map 009 (the automap on) loaded by a new run and by a run playing another map (F7's path), each going on as the saving run did for 500 vblanks (the game state lines, paraLLEl-GS's pictures byte for byte) |
| `vu1_disasm.py` | the game's VU1 programs as text |
| `map_tour.py` | every map visited and pictured without playing there, with a GS capture and census of each (below) |
| `motion_compare.py` | the PC renderer against paraLLEl-GS in motion (below): menus, a pick-up, combat, spells, creatures' effects, fades, under water, each played by a pad script (`tests/motion/`) by both renderers and compared vblank by vblank (as `tour_compare.py` measures), at 1x and at 4x with native widescreen; the PC renderer's pictures between at 240 checked with `flash_find.py --frames` and its game frames there against the 30 run's |
| `hold_compare.py` | the scene renderer's frame held back (docs/plans/scene-slice4-results.md, "4b") against drawn as its scene closes: every `motion_compare.py` case played twice by the scene renderer (`KF4_SCN_HOLD_FRAME=1` and `0`, with `KF4_INTERP_T0=1` so frames are held under a pad script), at 1x and 2x widescreen, 60 and 240, each picture compared byte for byte; needs numpy (motion_compare's). `--var KF4_SCN_AHEAD_FORCE --stat "packets ahead forced"`: the packets taken ahead of the pictures between (docs/plans/scene-slice5-results.md) as far as they go, against not |
| `pipeline_seed.py` | the PC renderer's pipeline seed (`port/gs/pc/pipeline_seed.bin`): `map_tour.py` over every map and `motion_compare.py`'s cases, at 1x and with MSAA, the native draws on and off, each run's pipeline list kept (`map_tour.py --keep-lists`) and merged; made again when Granite's pipeline state changes (the renderer says the seed is not of its layout) or new draw states appear |
| `tour_compare.py` | two map tours (`map_tour.py`, `--renderer pc` and `--renderer gs`, same maps and views) compared picture by picture, ranked by what is drawn differently (a blurred difference, so filtering and edges count little); `report.md`, `report.html` with the worst views as PC / reference / difference |
| `gsreplay/` | `kf4-gsreplay` (below) |
| `replay_compare.py` | a GS capture of many frames drawn by the PC renderer (at a player's settings) and by paraLLEl-GS, compared frame by frame (`kf4-gsreplay`, below): the frames where the PC renderer's picture differs most, and with `--decal-losses` where decals lost a near tie (below) |
| `flash_find.py` | the pictures of a flash recorder folder (F8) or a `--dump-frames` folder that differ from both their neighbours, a list and a contact sheet (`flashes.png`); docs/port/README.md, "Debugging" |
| `layer_inventory.py` | the game's full-screen layers in a tour of play (scenes, actions, `KF4_FX`, every map): each one's GS state, its GPU time by draw (`kf4-gsreplay --gpu-draws`), what reads each channel it writes, and its class (clear, dead, foldable into scanout, must stay in memory); real-window totals at the owner's settings; `report.md` (below; docs/plans/fullscreen-layers-inventory.md) |
| `world_plain.py` | the world's GPU time drawn the GS's way and with each GS-exact mechanism switched off (`KF4_PC_*` switches, the prototype plain path `KF4_PC_PLAIN_WORLD`, RADV's compression and HiZ off), a few scenes at 1x, 2x and 4x in a real window with the owner's settings, by `layer_inventory.py`'s live step (below; docs/plans/world-plain.md) |
| `scene_census.py` | what each game frame hands the renderer at a few scenes (native draws by kind, GIF primitives by path and class, the draws in order and their runs, uploads, copies, DATE's fragments), headless with the PC renderer's frame census (`KF4_PC_FRAME_CENSUS`); docs/plans/scene-renderer-design.md |
| `scene_scenarios.py` | the scene renderer (`--renderer scene`) against the PC renderer in `layer_inventory.py`'s scenarios, headless, picture by picture at each scenario's moments (2x with native widescreen); `--gs` (with `--scale 1`) plays paraLLEl-GS too and measures both against it; `--reference-exe EXE` compares against that build's scene renderer instead (an earlier slice's: byte for byte); docs/plans/scene-slice1-results.md, scene-slice2-results.md |
| `depth_ties.py` | the PC renderer's developers' counters read (`map_tour.py --ties`, `motion_compare.py --ties`): the depth rule's ties by map and by class of draw, the draws with the most, and the world's fragments a pixel; `--scenes` the overdraw and draw order by depth mode (below; docs/plans/depth-by-layers.md) |
| `screen_fx_test.py` | every full-screen and screen-reading effect made to happen headless (`KF4_FX`, saves, teleports, pad presses), measured at the owner's settings at 240 (pictures and game frames a second, cuts, hitches, GPU time a picture) and its pictures between checked with `flash_find.py --frames`; a table of OK / slow / corrupt (docs/port/SCREEN_EFFECTS.md) |
| `zfight_test.py`, `zfight_add_spot.py` | z-fighting at the spots where it was seen (`tests/zspots/*.json`: map, position, heading; no game data), headless: tiny turns, steps and a short walk, and the pixels that jump away from both neighbours counted; PASS / FAIL a spot with an evidence picture. `zfight_add_spot.py NAME` adds a spot from the newest quick save (docs/port/ZFIGHT_TEST.md) |
| `vif_replay_all.py` | `kf4-gsreplay --mode vif` over many captures (files, or folders searched for `*.kf4gs`), a line each and a total; `--vu1 check` compares the HLEs of the VU1 programs (0x000, 0x280, 0x5A0, 0x700; 0x276 passes packets through) with the interpreter on all of them (fails if anything differs) |
| `pace_test.py` | the pictures a second in play at a few maps at the owner's settings, and the GPU time a picture (docs/port/RENDERER.md, "Measuring the pace") |
| `sampler/` | `profile.py` plays to a map as `pace_test.py` does and profiles every thread's functions, then prints each thread's CPU time and the busiest's (or `--thread`'s) functions by inclusive and self share; on Linux the game samples itself (`--profile-at`) and the GPU's time a picture by kind of work (`--gpu-detail`: by shader state) follows, on Windows `kf4-sampler` samples from outside (stacks unwound from the program's `.pdata`). `stacks.py` reads a kept run again, or one saved from Shift+P (`--thread`, `--callers`, `--callees`, `--gpu`, `--exports`); docs/port/PROFILING.md |
| `build_compare.py` | two builds of the game played the same way at four spots: their pictures byte for byte, their state each vblank and F10's state dump the same (a release build against the developers' one: below) |
| `pgo_train.py` | a release build with profile-guided optimisation: built to write a profile, trained at four spots, built again with it (below) |
| `build_cost.py` | each thread's CPU time in play at a map for several builds, alternated, from `sampler/profile.py`'s runs, and the part of it not spent spinning (the game's thread waits for DMA in a spin); below |
| `make_font_letters.py` | the English letters of the sharper font drawn from IPA Mincho into `port/gs/shared/font_letters_data.h` (committed; made again only to change the letters): `python3 tools/make_font_letters.py path/to/ipam.ttf [--preview out.png]`; third_party/ipa-fonts/README.md, port/gs/shared/font_sheet.h |
| `record_input.py`, `record_input.bat` | a session recorded as it is played (`--record-input`, the player's settings and PC controls on) and replayed headless, byte for byte: to see what a player saw, and to bisect it (below) |
| `build_rev.cmake` | (the build runs it) writes `build_rev.h`, the commit a GS capture names |

## record_input.py: record a session, replay it, bisect

For a bug a player sees in play that no scripted run shows: the player records the session as they play it, and
a developer replays it headless, as often as needed, with any build.

```sh
python3 tools/record_input.py record --disc <image>         # the player: plays in the window, presses F8, quits
python3 tools/record_input.py replay <REC> --disc <image> --check        # in step? the same pictures twice?
python3 tools/record_input.py replay <REC> --disc <image> --exe <other build>/kf4 --out /tmp/r1   # bisecting
python3 tools/record_input.py replay <REC> --disc <image> -- --gs-capture-at 3616    # more options for kf4
```

On Windows without Python the player runs `tools
ecord_input.bat [kf4.exe] [disc]` instead (or double-clicks it,
with `KF4_EXE` and `KF4_DISC` set, or `kf4.exe` beside it): the same recording.

**record** copies what the run starts from out of the data folder into `REC/start` (`settings.json`,
`overlay.ini`, `quicksaves/`, `automap/`, `hands/`, `weapon_sets/`, `saves/`; nothing in the data folder is changed), then
starts the game in its window with the player's settings, `--clock 2026-01-01T12:00:00`, the newest quick save
loaded by itself (`--quick-load`; `--quick FILE` another, `--quick none` the title and the saves) and
`--record-input REC/input.pad`. The player plays as usual, presses F8 when the thing shows (the F8 pictures go
to `captures/` as always, and the vblank into the recording), and closes the game. `REC` is
`<data folder>/captures/input-YYYYMMDD-HHMMSS` (`--out`, or `KF4_REC` for the .bat), beside F8's and F9's
captures, which is what a player is asked to send. `REC/info.txt` names the program, the quick save, the clock and
the extra options. A recording holds the player's saves and settings: it is never added to the repository.

**replay** copies `REC/start` to `OUT/data` and runs the program headless and silent with `KF4_DATA_DIR` there and
`KF4_READ_SETTINGS=1` (the player's settings and mods, headless too), the same quick save and clock,
`--frame-rate 240` (`--frame-rate ''` for none) and `--input REC/input.pad`, to 60 vblanks after the recording
ends, with `--log OUT/log.txt`. It prints the F8 vblanks and dumps their pictures into `OUT` (`--at` other
vblanks; at 240 the pictures between too: `frame_NNNNNN_250.png` ...), the maps the player went through, and
fails if the program says `[pad] the replay is out of step`. `--check` replays twice (`OUT/a`, `OUT/b`) and compares
the logs (every vblank's state) and the pictures byte for byte. The compiled pipelines are kept between replays
(in the temporary folder's `kf4_replay_pipelines`): a first run with none compiles them as it goes, and a compile
that stalls the renderer for seconds can set off its GPU hang watch (exit 70; replay again).

What `--record-input` writes (`port/sdk/libpad.c`), one line per run of vblanks the game read the same:

```
1 - rx=128 ry=128 lx=128 ly=128 ts=1 run=0 224      <vblank> <buttons> sticks, twin-stick, run key, <vblanks held>
225 look -0x1.cd1a1e0000000p-4 0x1.015bf80000000p-6  a mouse turn port_mouse_look gave Player_Move (yaw, pitch)
1200 event weapon_swap 0                             a PC key the game sees: weapon_swap, game_save, game_load <slot>
# vblank 3616: F8 (flash record)                     F8 and the save-state keys, as comments
```

What is replayed: the pad's buttons and sticks as the game read them each vblank (the last reading of a vblank,
when the overlay, the full map or a pipeline wait reads it again), the mouse look's turns call for call as exact
floats, the Run key, twin-stick's shaping, the weapon swap key, and the quick
save and quick load keys (against the copy of the quick saves). What is not: save states (F5 / F7: written as
comments; a loaded one makes the rest go wrong), quick loads and saves made from the overlay's menu, settings
changed in the overlay mid-run (the replay has the settings of the start), F8 and F9 themselves, the sound, and
instant mouse look's turns of the pictures between (they go by the wall clock; the replay leaves instant mouse
look out, so its pictures between repeat but are not exactly the ones the player saw; the game frames are). The
player should avoid the first three.

Determinism: a recording is the game's input per vblank, so it replays the same headless or in a window and at
any frame rate, from the same start: the same quick save, clock, saves and data folder (`REC/start`). A
build that changes what the game does (game code, input, the mouse look) can go out of step: each recorded turn is
checked against the vblank it is given back in, and the first that is late is reported. A build that changes
only the drawing replays in step, which is what makes a recording good for bisecting a renderer bug: replay with
each build (`--exe`) and look at the pictures at the F8 vblank. Checked 2026-11-15 (Windows, headless, the owner's
settings, 240): a windowed recording by the owner of map 0 to the temple area and back (0:00 -> 1:63 -> 0:00,
4191 vblanks) replays in step, twice, logs and the pictures at its F8 vblank byte for byte the same; it found the
dark rectangle of #143 (twin marks carried across maps) by bisecting.

## A release build

The presets developers use are `RelWithDebInfo` (-O2) without LTO, and on Linux with frame pointers for the
profiler. The release presets keep -O2 and the debug information, and add:

- `KF4_FRAME_POINTERS=OFF` (already off on Windows);
- `KF4_LTO=ON`: ThinLTO with clang, gcc's LTO with gcc, for everything but the save state's image. The game's
  objects, ico-audio's and the state markers stay plain objects: an LTO linker lays the data it compiles out after
  every plain object, which would take the game's globals out from between the markers (`state_begin.c`), and the
  game's code keeps its own flags (`KF4_GAME_FLAGS`, `KF4_EE_FLOAT_FLAGS`) with nothing inlined across.

```sh
cmake --preset windows-clang-release            # or linux-clang-release, linux-gcc-release
cmake --build out/windows-clang-release
```

PGO on top (clang or gcc; Windows needs `-D SDL3_DIR=...` as for any configure):

```sh
python tools/pgo_train.py --preset windows-clang-release --disc <image> [-D SDL3_DIR=...]
```

It configures `out/<preset>-pgo` with `KF4_PGO=generate`, builds `kf4`, plays the four spots headless and silent for
3000 vblanks each (the program writes its profile as it stops: `kernel.c`), merges the profiles (clang:
`llvm-profdata`, beside the compiler or `llvm-profdata-<version>`), and builds the same folder again with
`KF4_PGO=<the .profdata>` (gcc: `KF4_PGO=use`, the `.gcda` files beside the objects).

A release build must be the same game: compiler differences are bugs here. Check every new release build
against the developers' one before trusting it:

```sh
python tools/build_compare.py --exe out/windows-clang/kf4.exe --exe out/windows-clang-release-pgo/kf4.exe --disc <image>
```

Each spot (maps 000, 009, 009 place 2, 063: `--teleport 1900:MAP:PLACE --invincible --frame-rate 240 --headless`)
must say SAME: the pictures at three vblanks (and their pictures between), the state line of every vblank (`--log`)
and F10's state dump (`KF4_DEBUG_AT=state@`; host function pointers in it are compared by name, since PGO moves the
game's functions). The unit tests too, `libm_test`, `vu1_test` and `int_divide_test` (not in ctest) among them.
`int_divide_test` is built without LTO: it checks the operand forms of the game's code, which is never LTO'd.

Then the speed (on a quiet machine, the GPU idle):

```sh
python tools/build_cost.py --map 9 --runs 5 --disc <image> --exe out/windows-clang/kf4.exe \
    --exe out/windows-clang-release/kf4.exe --exe out/windows-clang-release-pgo/kf4.exe
python tools/pace_test.py --exe ... --maps 9 --hz 500 --runs 3      # pictures a second uncapped, each build in turn
```

## kf4-gsreplay

Draws a GS frame capture again: the file F9 writes in the game (or `--gs-capture-at`), into the
`captures` folder of the data folder (docs/port/README.md, "Debugging"). It is built with the game
when paraLLEl-GS is checked out: `out/<preset>/kf4-gsreplay`. A capture is read by any build, so a
capture the player made is replayed in a build with more logging, a fix, or a change of the VU1
interpreter.

```
kf4-gsreplay FILE.kf4gs [options]
```

| Option | What |
|---|---|
| `--info` | the capture's vblanks, the game's state line (map, position), the build, the picture options, and its records |
| `--mode gif` | (the default) the GIF packets as the GS had them |
| `--mode vif` | the VIF1 stream through the VU1 interpreter, which makes the PATH1 and PATH2 packets again; each is compared with the recorded one, and for each that differs the first vertices that differ are printed (`--diff N`, 8 by default; 0 for none); then VU1's runs, instructions and cycles (stalls included) for the capture |
| `--vu-round nearest` | with `--mode vif`: VU1's float arithmetic rounds to nearest instead of toward zero (a hypothesis to test: what moves) |
| `--vu1 interp\|hle\|check` | with `--mode vif`: who runs the VU1 programs (0x000, 0x280, 0x5A0, 0x700; 0x276 passes packets through): the interpreter (the default), the HLE (`port/platform/vu1_hle*.c`), or both, compared run by run (data memory, packets, registers, flags, clock; the differences on stderr, then `VU1 check: N runs compared, M differ`). VU1's time for the capture is printed too, without the GIF hand-offs |
| `--frames` | each scanout's picture as `frame_NNNNNN.png`, as `--dump-frames` writes it (byte for byte the same) |
| `--out DIR` | where pictures go (the current directory) |
| `--list` | every draw, one a line (below) |
| `--draw N`, `--draws A-B` | the frame buffer the draw drew into, after draw N or after each of A to B, as `draw_NNNNN.png` |
| `--zbuf` | with them, its Z buffer too, as `draw_NNNNN_z.png`: grey from the nearest to the farthest Z in the picture, black where Z is 0 |
| `--pixel X,Y` | the draws whose box and scissor cover frame buffer pixel (X,Y), each with the pixel's colour (ABGR) and Z after it, and `changed` where either changed; `--fbp N` keeps only those drawing into frame buffer page N |
| `--vertices N` | draw N's vertices: X, Y (12.4, as sent, before XYOFFSET), Z, RGBA, STQ, UV, fog, and whether it kicked a primitive |
| `--events` | after the draws, the capture's transfers (each TRXDIR write: uploads, local copies such as the frame's to 0x2BC0, read-backs; BITBLTBUF, TRXPOS, TRXREG) and vblanks (the buffers DISPFB1 and DISPFB2 show), each with the draw it comes before: `event transfer rec R before D dir 2 sbp ... dbp ... src X,Y dst X,Y size WxH`, `event vblank rec R before D vblank V field F dispfb1 fbp ... dispfb2 fbp ...` |
| `--gpu-draws FILE` | (pc) the GPU's time by draw: each draw is numbered for the PC renderer as it is sent, and every GPU interval (each batch one, `KF4_GPU_DRAWS`) is written to FILE as `interval picture work state first_draw last_draw ns`: a batch's draws (a batch of several shares its time), other work the draw it came before (a local copy, an upload). With `--repeat`, the numbers start again at 0 each time. Batches are made as without it |
| `--compare OTHER` | another capture's draws against these, one by one: the GS state, and the vertices (the first that differs, and the largest Z difference) |
| `--compare-mode gif\|vif` | compare this capture with itself in that mode (with `--vu-round`: the VU1 variant against the recorded packets, draw by draw) |
| `--limit N` | how many differing draws `--compare` prints (50) |
| `--renderer gs\|pc` | who draws: paraLLEl-GS (the default) or the PC renderer, the one players get. A capture holds no settings of the PC renderer's: give the player's with `--scale X[xY]` (`native_scale`; with `native_widescreen` and `widescreen` X is `(4 * scale + 2) / 3`: 6x4 at 4), `--aniso N`, `--lod ps2\|scale\|full`, `--2d ps2\|smooth`, `--antialias off\|fxaa`. The replay waits until the scale is in effect before the capture's first draw. `--draw`/`--draws` read the GS's memory, which the PC renderer does not keep at the PS2's size: use `--frames` |
| `--end-picture` | `--frames`, and at the end `frame_end.png`: the buffer the last draw drew into, shown as the last vblank shows its buffer. A capture at 30 frames a second ends before the frame it drew is shown (its own vblanks show the frame before) |
| `--repeat N` | the stream drawn N times over by the same renderer, its state kept between (with `--end-picture`, `frame_end_NNN.png` after each): whether what the renderer carries from frame to frame changes the picture |
| `--pixel X,Y` with `--renderer pc` | the PC renderer's trace of one surface pixel of GS pixel (X,Y) of the frame buffer at page `--fbp N` (0 by default); `--sub SX,SY` picks the surface pixel in the GS pixel's block of the scale (its middle by default). For each draw whose box covers it: every fragment shaded there (its primitive, its Z and slope, the samples it covers, its colour, whether the alpha test or DATE discarded it, whether it was drawn as a decal), each covered sample's Z against the Z stored before (from the draw's ZTST: `passes` or `FAILS`), and the samples' Z and colour after the draw, read back on the GPU. Its draws are depth tested after their shaders (the picture is the same), and each read-back is a pass of its own |
| `--decal-losses` | (pc) each decal (a blended draw tested GEQUAL: `pcr_canvas.cpp`'s `is_decal`) drawn again where it lost its depth test by less than 16 Z, adding 16 of red and blue there per decal: the picture less the same replay without the option shows where |

A **draw** is a run of vertices in one GIF packet with no GS register written between them other than
a vertex's own (RGBAQ, ST, UV, FOG). `--list` gives, for each:

```
   412 rec   3120 p1 prog 000 kick   2 tstrip tex abe fog ctx0 v   30 k   10 box  -12, 180- 655, 300 z 0000001C-00FFFFC5 | FRAME fbp 0 fbw 10 psm 00 fbmsk 00000000 | ZBUF zbp 280 psm 3A zmsk 0 | TEST ate 1 atst 6 aref 00 afail 0 date 0 datm 0 zte 1 ztst 2 | ALPHA 0101 fix 80 fba 0 pabe 0 | TEX0 ... TEX1 ... CLAMP ... | SCISSOR 0,0-639,447 XYOFFSET 1728,1824
```

its number, the capture record it starts in, the path, for PATH1 the VU1 program's address (in
instructions, as `tools/vu1_disasm.py` numbers them) and which of its XGKICKs, PRIM (type, `tex`, `abe`
alpha blending, `fog`, the context), the vertices and the drawing kicks, the box in frame buffer pixels,
the Z range, and the GS state it drew with: FRAME, ZBUF, TEST (alpha test, destination alpha test, the
depth test: ZTST 0 never, 1 always, 2 greater or equal, 3 greater), ALPHA (A B C D and FIX), FBA,
PABE, TEX0, TEX1, CLAMP, SCISSOR and XYOFFSET; then, textured, `| UV u0,v0-u1,v1` (the texels its vertices
name: UV, or S/Q and T/Q times the texture's size), and `| RGBA AABBGGRR` (its first vertex's colour, RGBAQ's
low word as a number: alpha first, red last), and `| DTHE d COLCLAMP c` (dithering on, and the colour clamped rather
than wrapped).

Pictures need a Vulkan GPU; `--info`, `--list`, `--vertices` and `--compare` do not.

### Example: a flicker on the water

The healing spring's water flickers with dark lines as the view moves. Stand where it shows, press F9
(a few times: one may catch it and one not), then:

```sh
kf4-gsreplay gs_a.kf4gs --frames --out a              # find the capture that shows the lines
kf4-gsreplay gs_a.kf4gs --list > a.txt                # the draws: find the water's (its TEX0, its box)
kf4-gsreplay gs_a.kf4gs --pixel 300,260 --fbp 0       # which draws write a pixel on a line, their Z and ZTST
kf4-gsreplay gs_a.kf4gs --draws 600-640 --zbuf --out a    # the picture and Z after each of them
kf4-gsreplay gs_a.kf4gs --vertices 612                # the two passes' vertices: is Z the same?
kf4-gsreplay gs_a.kf4gs --compare-mode vif --vu-round nearest   # does VU rounding move their Z?
```

### Example: the PC renderer against paraLLEl-GS

For something the PC renderer draws and paraLLEl-GS does not (a flicker that shows in some frames only), F9 can
record more than one frame: start the game with `KF4_GS_CAPTURE_VBLANKS=240` (4 seconds; 1 to 600, about 5 MB of
memory a vblank while it records), press F9 just before doing what shows it, and keep doing it. A capture made with
the PC renderer holds its GS memory and registers at its start, so its first frames are drawn from where the game
was. Then, at the player's settings (here 4x with native widescreen, FXAA):

```sh
python3 tools/replay_compare.py gs_b.kf4gs --out b --pc "--scale 6x4 --aniso 16 --lod scale --2d smooth --antialias fxaa" --decal-losses
# b/report.txt: each frame's difference and decal losses; b/worst_*.png: PC | paraLLEl-GS | difference
kf4-gsreplay gs_b.kf4gs --renderer pc --scale 6x4 --pixel 300,242 --sub 3,0   # one pixel of a bad frame
```

## map_tour.py

Visits every map of the game (the 64 `NNN.map` files of KF4.DAT) and pictures it, without playing
there: for checking the drawing of places nobody has reached yet, and for a census of what the maps
draw with.

```sh
python3 tools/map_tour.py --disc kfiv_us.iso --out /tmp/tour            # all maps
python3 tools/map_tour.py --out /tmp/tour --maps 0,99,112 --positions 3  # some, more places each
```

How it gets there: a quick save is the game's own save image (docs/port/README.md, "Quick saves"), so
the tool writes one for any map and place.
- It takes a real one once: a new game (`tests/newgame.pad`), `--quick-save-at` in the first map.
- For each view it writes a copy with the image's map tag, position and heading changed (and the
  image's byte sums and the file's hash made again), marked made up: the PC block's flags bit 1. A made-up
  file loads as any quick save, then the game puts the player on the floor nearest under the position
  (`Collision_FindFloorBelow`, as a dropped item is put down; else the nearest over it), and works the
  neighbouring map out from there (`game/quicksave.c`). The game never writes the bit.
- The places come from the map file (read from the disc): the map's own start (`MapSlotUnk08`'s
  `player_start_*`), then its fixed items that lie on a record's floor (where the player can pick them
  up), then the middles of the records' biggest floor triangles. The floors are the records' collision
  meshes (`CollisionMesh` floor strips, as `Collision_RelocateMesh` reads them; records that are turned
  are left out); a place over a hazard strip (lava, poison) is left out, and each is 1500 or more from
  the others. Not the entity spawns: a player put on one dies at once. `--positions` places (2), each
  at `--headings` headings (4).
- Each map is one run: headless with the GS on, `--quick-load A,V:B,...` loads its views one after
  another (100 vblanks each: the load, the fade in, the picture), `--dump-frames` writes each view's
  picture and `--gs-capture-at` captures the first. `kf4-gsreplay --list` lists the capture's draws
  and `--mode vif` runs its VIF1 stream through the VU1 interpreter again (VU1 cycles for the frame,
  and packets that come out otherwise). Maps run `--jobs` at a time (3).

What it writes into `--out` (outside the repository: the pictures, saves and captures are the game's):
- `NNN/sheet.png`: the map's views side by side, each labelled with its place and heading, and in
  yellow what was odd about it;
- `NNN/pictures/`, `NNN/log.txt` (the per-frame log), `NNN/stderr.txt`, `NNN/capture.kf4gs` and
  `NNN/draws.txt` (its `--list`);
- `report.html` (the table and the sheets), `report.md` (the table, then the census: each VU1 program
  and each GS state combination of PRIM, TEX0 PSM, ALPHA, TEST, ZBUF and FRAME, with the maps that
  draw with it), and `report.json` (everything).

A view is flagged when the load did not happen or came late, no floor was found, the player is not on
the map, has a NaN position or is dying (an enemy, a trap), the game's status is not plain play, or the
picture is black or more than half one flat colour (a void, or a dark doorway). The map is flagged for
a `[todo]` the new game did not print, `[fatal]`, a hung kernel, a quick save message, a draw whose
TEX0 names no PSM, more than two frames' worth of VU1 cycles, or VIF1 packets that replay differently.
Flags are leads, not verdicts: look at the sheet.

## motion_compare.py

The map tour's comparison for what moves: each case is a pad script played after `tests/newgame.pad`
(`tests/motion/*.pad`, vblanks counted from the start of the run) with the options and `KF4_FX` it needs
(`game/screen_fx.c`: `spell=N` keeps spell N in hand to cast, `creature=ROW[:DIST]` puts the player DIST before
a creature of that chr record once it has been there 60 frames, `water` and `drainramp` force the under-water
and MP drain screens). The same input and vblanks give the same game, whoever draws, so the two renderers'
pictures of a vblank show the same moment; the per-frame log (`--log`) at each compared vblank is checked to be the
same in both runs, and a vblank where it is not is left out and named.

```sh
python3 tools/motion_compare.py --disc kfiv_us.iso --out /tmp/motion                 # every case, 1x and 4xw, 240
python3 tools/motion_compare.py --out /tmp/motion --cases menus,combat --profiles 1x --no-240
python3 tools/motion_compare.py --cases list
python3 tools/motion_compare.py --out /tmp/motion --profiles 1x --no-240 --renderer scene --pc-extra "--native-draw on --frame-rate 30"
```

For each case and profile (`1x`: `--native-scale 1 --no-native-widescreen`; `4xw`: `--native-scale 4 --widescreen
--native-widescreen`, gs with `--widescreen`, the 4x pictures brought down to 640 x 448), three
headless runs one after another: gs at 30 dumping the case's vblanks, pc the same, pc at `--frame-rate 240`
(left out with `--no-240`). Pictures are measured as `tour_compare.py` does (mean, share over 24 levels, the same
after a 3 x 3 blur). At 240, `flash_find.py --frames` judges each picture between against its game frames, and
each vblank where the log's frame counter moves on (a game frame's own picture) is compared with the 30 run's
picture of it. The screens the game draws as fields (title, menus) differ from gs at every edge by design: gs shows
one field a vblank, pc the upper field each vblank (c043b057), so their numbers are high without anything missing;
look at the pairs.

Into `--out`: `report.md` (a line a case and profile, then each one's worst vblanks), `report.json` (every
vblank), `evidence/` (the worst `--top` pairs of each, 640 wide: PC | GS | the difference x4, red over 24; and
flash_find's sheet of flagged pictures between). The runs' pictures (about 20 MB each at 4x) are deleted as it goes
(`--keep` keeps them). A full run (12 cases, both profiles, 240) is about an hour and a half; one game at a time.

## layer_inventory.py

The inventory of full-screen layers (docs/plans/fullscreen-layers-inventory.md), made again after each step of the
renderer's redesign. Each **scenario** is a piece of play: a New Game and a pad script (`tests/newgame.pad`,
`tests/motion/`), teleports, `KF4_FX` (`game/screen_fx.c`: forced effects, `mapchange=N`, `savepoint`), or a quick save
made up at a map's start (`--maps`, map_tour's). `--scenarios list` names them.

```sh
python3 tools/layer_inventory.py --out /tmp/layers --disc kfiv_us.iso                     # capture, time, report
python3 tools/layer_inventory.py --out /tmp/layers --maps all --scenarios ""               # every map's start
python3 tools/layer_inventory.py --out /tmp/layers --steps live                            # real window (below)
python3 tools/layer_inventory.py --out /tmp/layers --steps time --scales 3x2,2x1,6x4 --scale-scenarios start,pond
python3 tools/layer_inventory.py --out /tmp/layers --steps report --scales 3x2,2x1,6x4
```

Steps (`--steps`; each skips what is done, `--force` does it again):
- `capture`: each scenario headless with native draw off (the GS stream as the PS2 had it: every pass a GIF packet),
  `--widescreen`, a GS capture of 4 vblanks at each of its moments (two game frames at 30, so that the next frame's
  reads show), `kf4-gsreplay --list --events` of each, a picture of each.
- `list`: the lists made again from the captures (after kf4-gsreplay's list changes).
- `time`: each capture replayed by the PC renderer at `--scales` (3x2: the owner's 2x with native widescreen, 1920x896
  a buffer; 2x1: 1x; 6x4: 4x), FXAA, 16x anisotropic, `--repeat` times with `--gpu-draws`: each draw's GPU ms, the
  median of the repeats after the first two.
- `live`: each scenario with a live moment in a **real window** with the owner's settings (a copy of
  `~/.local/share/holyland` with `frame_rate` set, `KF4_READ_SETTINGS=1`, native draw as the owner has it), at
  `--live-rates` (60, 30), with a `--live-seconds` profile (`KF4_GPU_DETAIL=1`): the GPU's ms by kind and shader state.
  Takes the screen: about 45 s a run. Run it on mains power, with nothing else on the GPU.
- `report`: `report.md` and `report.json`.

How a layer is found and judged (the analysis, in the tool):
- A **layer** is a run of draws in one GS state (all but box, vertices and an untextured TEX0) of sprites or a few
  vertices whose boxes cover 90% or more of their buffer's scissor: of a buffer some vblank shows (the frame), or of
  another target 256 x 256 or more (the reflection). A local copy (BITBLT, `GsDraw_CopyFrame`) is one too.
- What a draw **reads and writes** follows the GS: the alpha test's AFAIL, FBMSK, ZMSK and ZTST, a blend reading Cd
  or Ad, DATE, a texture's pages (from its UV), a copy's source; it **kills** a channel (writes it at every pixel
  whatever was there) only as one sprite with no blend, no Z test but ALWAYS, no DATE and no alpha test that the pixels
  decide. Each channel a layer writes is followed forward in 8 x 8 pixel cells through the draws, transfers and
  scanouts after it, the next game frame's included, until killed. A channel left at the capture's end is taken as
  killed if the draws before the layer in its frame killed it (the buffer's next use begins as its frame did).
- A layer is judged only in a game frame whose buffer a vblank in the capture shows.
- **Classes**: (a) a flat colour or Z over the whole buffer, nothing tested (a fast clear once buffers are images of
  their own); (b) dead: everything it writes is written over before anything reads it; (c) foldable into the scanout:
  a per-pixel colour op (untextured, or reading its own buffer at its own pixel), its alpha and Z unread, its colour
  read only by the scanout and by later such layers, and no other draw over it before the scanout; (d) must stay in
  memory: read as a texture, copied, or read by the next frame; (e) other, with the reason (alpha only, foldable at its
  place but drawn over by the HUD, reading a texture at other pixels).
- The **name** is told from the state (`layer_name`: the swap's clears, `GsDraw_ClearDepth`, `GsDraw_SetScreenAlpha`,
  `GsDraw_FillScreen` and its colour, `Game_ApplyBrightness` and `Game_TintScreen` by colour, the previous frame, the
  wobble, the menus' grab): check a new one against the decomp.
- **Live**: a game frame's ms is twice the 30 run's ms a picture, a picture between's twice the 60 run's less the 30
  run's, by shader state (when both ran 30 game frames a second).

Into `--out`: `scenarios/NAME/` (the captures, their lists and `--gpu-draws` files, the pictures, the live profiles),
`report.md`, `report.json`.

## world_plain.py

What the world costs on the GPU (docs/plans/world-plain.md): `layer_inventory.py`'s scenes (`start`, `pond`,
`palace`, any with a live moment) played in a **real window** with the owner's settings by its live step, once for
each **variant** at each native scale. A variant is a set of developers' switches, each off the exact path (the
picture may differ; for timing only): `--variants list` names them. `--rounds` runs the whole matrix again in the same
order, so that a drift of the GPU's clocks shows as a spread, not as a variant's cost; a variant's files already made
are kept (delete them to run it again).

```sh
python3 tools/world_plain.py --out /tmp/world --disc kfiv_us.iso --rounds 2                 # 3 scenes, 7 variants, 1x/2x/4x
python3 tools/world_plain.py --out /tmp/world --scales 2 --variants gpu_nodcc,gpu_nohiz,plainworld,plainopaque
python3 tools/world_plain.py --out /tmp/world --scales 2 --rates 60 --variants exact,gpu    # pictures between
python3 tools/world_plain.py --out /tmp/world --report-only --variants exact,gpu
python3 tools/world_plain.py --out /tmp/world --scales 2 --rounds 2 --variants unorm,scene  # the scene renderer
python3 tools/world_plain.py --out /tmp/world --scales 2 --rounds 2 --variants unorm,scene_ref,scene \
    --exe-for scene_ref=/path/to/slice1/kf4                                                 # ... against an earlier build's
```

- **The world**: the native draws (programs 0x000, 0x280 the skinned models, 0x700), less program 0x000's DATE draws
  (the entity pool's) and the sky (read from a target's copy), with exact Z's planes pass. Every figure is GPU ms a
  game frame: the run's GPU time over its game frames (at 60, a game frame and a picture between).
- The plain path, `KF4_PC_PLAIN_WORLD=1` with `KF4_PC_DEPTH=gpu` (`pcr_canvas.cpp`): the world's batches drawn into
  images of their own, the size of the surface they were for, optimal layouts, attachment use alone, cleared by their
  pass's load op after each clear of the frame buffer; `=unblended` draws them without their blend too, `=opaque`
  without their blend in the frame's surfaces. The picture lacks the world (or its see-through parts): timing only.
  `--stats` counts its batches and clears.
- A run is about 14 s (4 s profiled, from `layer_inventory.py`'s base quick save); the full matrix of 3 scenes, 7
  variants, 3 scales and 2 rounds about 35 minutes. Mains power, nothing else on the GPU.

Into `--out`: `scenarios/NAME/live_VARIANT_sSCALE_rROUND_RATE.*` (the profile, the log, stderr, the result),
`world_plain.md` and `world_plain.json` (the medians by scene, scale, rate and variant).

## scene_census.py

What each game frame hands the renderer (docs/plans/scene-renderer-design.md, "The frame as a scene"): each scene
(`layer_inventory.py`'s scenarios: 099's start, the pond, the palace, combat, under water, the menus, the title) played
headless at 30 with native draw on at 2x, with the PC renderer's frame census (`KF4_PC_FRAME_CENSUS=FILE`,
`KF4_PC_FRAME_CENSUS_AT="A-B"`: `port/gs/pc/pcr_frame_census.h`, one JSON line a frame) and its overdraw counter
(DATE's fragments). `census.md` is each scene's median frame; `census.json` the same as data.

```sh
python3 tools/scene_census.py --out ~/Documents/holyland-measurements/DATE-census --disc kfiv_us.iso
python3 tools/scene_census.py --out ... --report-only
```

The title and the menus' loops give their pictures no game vblank: their frames are all counted, those without native
draws kept.

## depth_ties.py

The depth rule's ties and the world's overdraw (docs/plans/depth-by-layers.md), from the PC renderer's developers'
counters (`pcr_canvas.cpp`'s `Counters`):
- `KF4_PC_TIES=FILE` (the depth rule on): every draw the rule tests counts what it decided for each fragment in a slot
  of its own (`pcr_prim.h`'s `pcr_count_ties`): tested, passed, the ties (within the rule's tolerance) passed at the same
  depth exactly, passed from behind or ahead, lost; those lost or passed within 2 Z past the tie; its Z's range. FILE's
  lines: `V` a vblank's totals, `W` a program's, `T` a draw's (its program, kind, owner, part, GS state and counts) at
  the vblanks `KF4_PC_TIES_AT` lists (every one without it), only those with ties unless `KF4_PC_TIES_ORDER=1`.
- `KF4_PC_OVERDRAW=FILE`: each native batch's fragment shader invocations (a pipeline statistics query), `Q` lines by
  program, frame buffer and kind a vblank.
- The game's canvas only (not the pump's); a kept walk's runs are not drawn while counting. Slow: developers' only.

```sh
python3 tools/map_tour.py --out /tmp/t --positions 3 --ties --renderer pc \
    --extra "--native-scale 1 --no-native-widescreen --native-draw on --frame-rate 30"   # NNN/ties.txt, overdraw.txt
python3 tools/motion_compare.py --out /tmp/m --profiles 1x --no-240 --ties --pc-extra "--native-draw on --frame-rate 30"
python3 tools/depth_ties.py /tmp/t            # /tmp/t/depth_ties.md and .json
python3 tools/depth_ties.py --scenes DIR      # DIR/MODE/SCENE/{overdraw,ties}.txt: invocations and passes a pixel by mode
```

`motion_compare.py --pc-extra "ARGS"` adds arguments to its pc runs (`--native-draw on`: its runs read no settings, so
the native draws are off without it). The depth modes to compare (`KF4_PC_DEPTH`): `rule` (the default), `exact`,
`gpu`, `layers` (the GPU's depth, Z clamped at each pixel, the decals `KF4_PC_LAYER_BIAS` nearer; `KF4_PC_LAYERS`,
`KF4_PC_PIXEL_CLAMP`) and `unorm` (the GPU's depth in the GS's 16-bit whole steps, `D16_UNORM`, DATE by a copy);
`world_plain.py`'s variants `layers` and `unorm` time them, and `gsexact` (`KF4_PC_DEPTH=gsexact`: exact Z's whole Z alone
at each pixel, the GS's arithmetic in the shader as a ceiling: docs/plans/scene-renderer-design.md).

## kf4-layerbench: what a full-screen layer costs on this GPU

`out/<preset>/kf4-layerbench [--size WxH] [--pictures N] [--layers N] [--only CASE] [--target TARGET]` (tools/layer_bench; built where
`glslangValidator` is). No game and no disc: plain layers on targets of the PC renderer's size and formats (8 and 16
bits a channel, 1 to 8 samples), one state changed at a time, ms a layer by the clock. For telling the card's limit
from the renderer's doing; the results of 2026-11-14 are in `docs/port/PC_OPTIMIZATION.md`.

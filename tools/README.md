# tools

| Tool | What |
|---|---|
| `patches.py` | the PC edits to the decomp: `apply`, `save`, `upgrade` (docs/port/README.md) |
| `port_census.py` | per folder of the game code, the errors and inline asm left (docs/port/RULES.md) |
| `port_abi_check.py` | declarations that disagree with the definition |
| `boot_test.py`, `frame_test.py` | step 1's and step 2's checks: a headless boot to a new game, and its pictures |
| `vu1_disasm.py` | the game's VU1 programs as text |
| `map_tour.py` | every map visited and pictured without playing there, with a GS capture and census of each (below) |
| `motion_compare.py` | the PC renderer against paraLLEl-GS in motion (below): menus, a pick-up, combat, spells, creatures' effects, fades, under water, each played by a pad script (`tests/motion/`) by both renderers and compared vblank by vblank (as `tour_compare.py` measures), at 1x and at 4x with native widescreen; the PC renderer's pictures between at 240 checked with `flash_find.py --frames` and its game frames there against the 30 run's |
| `tour_compare.py` | two map tours (`map_tour.py`, `--renderer pc` and `--renderer gs`, same maps and views) compared picture by picture, ranked by what is drawn differently (a blurred difference, so filtering and edges count little); `report.md`, `report.html` with the worst views as PC / reference / difference |
| `gsreplay/` | `kf4-gsreplay` (below) |
| `flash_find.py` | the pictures of a flash recorder folder (F8) or a `--dump-frames` folder that differ from both their neighbours, a list and a contact sheet (`flashes.png`); docs/port/README.md, "Debugging" |
| `screen_fx_test.py` | every full-screen and screen-reading effect made to happen headless (`KF4_FX`, saves, teleports, pad presses), measured at the owner's settings at 240 (pictures and game frames a second, cuts, hitches, GPU time a picture) and its pictures between checked with `flash_find.py --frames`; a table of OK / slow / corrupt (docs/port/SCREEN_EFFECTS.md) |
| `vif_replay_all.py` | `kf4-gsreplay --mode vif` over many captures (files, or folders searched for `*.kf4gs`), a line each and a total; `--vu1 check` compares the HLEs of the VU1 programs (0x000, 0x280, 0x5A0, 0x700; 0x276 passes packets through) with the interpreter on all of them (fails if anything differs) |
| `pace_test.py` | the pictures a second in play at a few maps at the owner's settings, and the GPU time a picture (docs/port/RENDERER.md, "Measuring the pace") |
| `sampler/` | `profile.py` plays to a map as `pace_test.py` does and profiles every thread's functions, then prints each thread's CPU time and the busiest's (or `--thread`'s) functions by inclusive and self share; on Linux the game samples itself (`--profile-at`) and the GPU's time a picture by kind of work (`--gpu-detail`: by shader state) follows, on Windows `kf4-sampler` samples from outside (stacks unwound from the program's `.pdata`). `stacks.py` reads a kept run again, or one saved from Shift+P (`--thread`, `--callers`, `--callees`, `--gpu`, `--exports`); docs/port/PROFILING.md |
| `build_compare.py` | two builds of the game played the same way at four spots: their pictures byte for byte, their state each vblank and F10's state dump the same (a release build against the developers' one: below) |
| `pgo_train.py` | a release build with profile-guided optimisation: built to write a profile, trained at four spots, built again with it (below) |
| `build_cost.py` | each thread's CPU time in play at a map for several builds, alternated, from `sampler/profile.py`'s runs, and the part of it not spent spinning (the game's thread waits for DMA in a spin); below |
| `build_rev.cmake` | (the build runs it) writes `build_rev.h`, the commit a GS capture names |

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

PGO on top (clang or gcc; Windows needs `-D SDL2_DIR=...` as for any configure):

```sh
python tools/pgo_train.py --preset windows-clang-release --disc <image> [-D SDL2_DIR=...]
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
| `--compare OTHER` | another capture's draws against these, one by one: the GS state, and the vertices (the first that differs, and the largest Z difference) |
| `--compare-mode gif\|vif` | compare this capture with itself in that mode (with `--vu-round`: the VU1 variant against the recorded packets, draw by draw) |
| `--limit N` | how many differing draws `--compare` prints (50) |

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
PABE, TEX0, TEX1, CLAMP, SCISSOR and XYOFFSET.

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
```

For each case and profile (`1x`: `--native-scale 1 --no-native-widescreen`; `4xw`: `--native-scale 4 --widescreen
--native-widescreen --native-msaa 2`, gs with `--widescreen`, the 4x pictures brought down to 640 x 448), three
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

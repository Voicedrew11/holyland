# Handoff: step 1, the inventory of full-screen layers

**Done 2026-11-19: `fullscreen-layers-inventory.md`.**

2026-11-19. For the next agent. Read first, in this folder: `renderer-scene-not-gs.md` (the direction: this is its
step "Order 1") and `camera-whip-slowdown.md` (the measurements that led here). Also `CLAUDE.md` at the root.

## Why this step

The PC renderer replays the PS2's GS stream almost as the GS drew it. King's Field IV draws many layers over the
whole screen (clears, Z-only writes, the destination alpha's reset, tints, fades, the brightness pass, the previous
frame blended in, copies of the frame read back). The GS's on-chip memory made those nearly free; on a PC GPU each is
a full pass through memory. On the Vega 7 laptop they are about 4.6 ms of a 10.6 ms game frame, and they are drawn
again for every picture between.

The owner wants this done right, as a general change that helps every GPU, not hardware-specific tuning or "lower
your settings". Two redesigns come next, and both depend on facts nobody has gathered yet:

- **Separate, compressible targets** (each displayed buffer its own image, optimal layouts, fast clears). Which
  layers become free on their own, and where the image boundaries must go.
- **Folding the screen-wide layers into one pass at scanout.** Which layers can be folded exactly, and which must
  exist in memory because something reads them back.

**This step is measurement and documentation only. Don't change the renderer.** New tools (scripts, a debugging
env var, more `--list` output) are fine.

## The deliverable

1. **`docs/plans/fullscreen-layers-inventory.md`** (new; keep it out of `docs/port/`). For every distinct
   full-screen (or near full-screen) layer the game draws:
   - **What it is.** The game function that draws it (`decomp/src/...`, and the patch if the PC changes it), what
     turns it on, and how often (every frame, per map, or situational).
   - **Its GS state.** FRAME (which buffer), ZBUF/ZMSK, TEST (alpha test, DATE, ZTST), ALPHA/FIX/PABE/FBA, TEX0
     (does it read the frame being drawn, the shown buffer, a copy at 0x2BC0, or a texture), its box, and the
     channels it writes (RGB, A, Z).
   - **Its cost.** GPU ms at the owner's settings (2x with native widescreen, 1920x896 a buffer) on the laptop's
     Vega 7, with the 9070 XT too if the owner can run it. Per game frame, and per picture between (the pump).
   - **What reads it.** Later in the same frame; the next game frame (the previous frame blended in, the shown
     buffer read); pictures between; copies (`GsDraw_CopyFrame` to 0x2BC0, the Dark Succubus' copy to 0x3540, the
     menus' grab at 0x3240). Which channels the readers use: a Z write nobody tests and an alpha nobody reads are
     dead.
   - **Its class:**
     - (a) a whole-buffer clear: a fast-clear candidate once buffers are separate images;
     - (b) dead: written over before anything reads it (some already known: `pcr_dead_writes.h`);
     - (c) foldable: a pure per-pixel colour op whose result is read only by scanout and by later foldable layers;
     - (d) must stay in memory: read back as a texture, copied, or read by the next frame;
     - (e) other (partial screen, feedback, needs a closer look).
   - **Exactness notes.** What folding or fast-clearing it would have to reproduce bit for bit: 8-bit rounding
     between layers, FBA, PABE, alpha-test fails, dithering, the order relative to the HUD and the arm.
2. **Summary tables at the top of that file:**
   - every layer, its class, its cost and how often it appears;
   - the ms a game frame each class accounts for in each scenario, so the owner can see what fast clears, dead-write
     removal and folding would each be worth;
   - the targets that share memory with the frame (the reflection's 256x256 view draws with its Z in the frame's Z
     buffer; the 0x2BC0 copy area is shared by several effects), since they decide where image boundaries go.
3. **The tool that made it**, in `tools/` (for example `tools/layer_inventory.py`), documented in `tools/README.md`,
   so the inventory can be made again after each redesign step. Raw output (captures, profiles, pictures) holds game
   data: keep it outside the repository.
4. **Open questions** the data could not settle, listed in the inventory.

## Starting points that already exist

- **The old screen-effects audit:** `git show 40a969d5^:docs/port/SCREEN_EFFECTS.md` (313 lines, removed when the
  docs were rewritten). It lists every full-screen and screen-reading effect found in the decomp, with triggers, GS
  memory, draws and what frame_interp does with each. It predates the native GS's removal: its native-draw and
  "modern" sections describe renderers that are gone. Use it as the starting list and check each entry against the
  current code.
- **`game/screen_fx.c` (`KF4_FX`)** forces the effects on for testing: `water`, `drain`, `drainramp`, `prev`, `aim`,
  `haze`, `depth`, `down`, `updown`, `shift` (only with `--no-whole-frame`), `fill`, `fade`, `tint`, `dim`,
  `bright40`, `brightff`, `spell=N`, `succubus`, `creature=ROW[:DIST]`, `copytex`, `mapview=N` (1-4 the picture maps, 5
  the King's Map). The comment at the top says what each sets.
- **`tools/screen_fx_test.py`** runs each effect headless; `--cases list` names them. Its default `--exe` is the
  Windows build: pass `--exe out/linux-clang/kf4`. Its real-scene cases need quick saves (`--spots`) that may not
  exist on this machine; ask the owner, or make them.
- **`tools/motion_compare.py`** cases and **`tests/motion/*.pad`**: `cast`, `combat`, `first_kill`, `herb_pickup`,
  `menus`, `stand`, `walk_on`. The `combat` case refers to `tests/motion/equip.pad`, which is **missing**:
  `first_kill.pad` does the equipping itself.
- **`tools/map_tour.py`**: every map (64), from made-up quick saves, headless, with a GS capture of each map's first
  view and a census from `kf4-gsreplay`. `--turn`, `--positions`, `--headings`, `--extra "ARGS"`.
- **GS captures:** `--gs-capture-at VBLANK[:N]` (F9 in game) and `out/linux-clang/kf4-gsreplay FILE --list`. The list
  has one draw a line with its full GS state and box (format in `tools/README.md`, "kf4-gsreplay"). It is the way to
  find full-screen draws and their reads: a draw whose TEX0 TBP is the FRAME FBP drawn reads the frame. `--pixel X,Y`
  shows every draw that wrote a pixel. `--renderer pc --scale 3x2 --antialias fxaa` replays a capture with the PC
  renderer at the owner's scale.
- **GPU cost:** `--profile-at VBLANK:SECONDS:FILE` with `KF4_GPU_DETAIL=1` (by shader state) or `=2` (each draw: a
  timestamp at every batch, which slows the GPU, so check its totals against `=1`), read with
  `tools/sampler/stacks.py FILE out/linux-clang/kf4 --gpu`. `KF4_GPU_LABELS=1` puts debug labels in the command
  buffers for RenderDoc or Radeon GPU Profiler. **Unsolved: matching a timed interval to a `--list` draw.** Find a
  way (labels carrying the draw's number, the same capture replayed under the profiler, or the order of draws within
  a frame) and write down how.
- **`port/gs/pc/pcr_dead_writes.h`** already decides, for the pump's walks, which writes are dead (read and write
  masks for each step, in GS pixels). Its rules and its test (`tests/pcr_dead_writes_test.cpp`) are a ready-made
  model for class (b) and much of the reads analysis.
- **What's measured already:** `camera-whip-slowdown.md` (map 099, by shader state) and `docs/port/PC_OPTIMIZATION.md`,
  "A Vega 7 at 1080p" and "The Vega 7 again" (map 009: one game frame's fills in order, two of them already turned
  into clears).

## The tour: a wide variety of play, not just views

The owner asked that the tour **do things**, so that the inventory sees the layers play produces, not only those of
standing in a map. `map_tour.py` only loads views; extend it (an `--actions` pad run after each map's quick load) or
write a tour of your own. Cover at least the following.

**Places** (every map once, plus these on purpose):
- All 64 maps at their start, each with a mouse spin (`KF4_DIAG_SPIN=6`) and a walk.
- Water with the reflection (the reflection's target and its Z in the frame's Z buffer).
- Under water: deep water on map 029, the Underground Canal; the deadly water's death screen.
- Map 035, the Guardian's Room, before its water is purified (the green fill instead of the tint).
- Lava, water and the flesh cavern (the scrolling textures' targets).
- The Queen's Palace, map 022, and the Giant Leader on map 034 (the MP drain's purple wobble).
- The Dark Succubus, map 038 (her body drawn from a copy of the screen).
- Dark places and haze, and the icy corridor.

**Actions** (in at least two different maps each):
- Walking, running, stick turns, mouse spins, looking up and down.
- Opening a door; picking up an item (`herb_pickup`).
- Equipping a weapon and swinging it: the weapon's trail (`first_kill`, `combat`).
- Getting hit (the hit jolt, the red fill), poisoned or darkened (the tint), knocked back (`aim`).
- Killing a creature and its death and reward (`first_kill`).
- Casting spells: at least 3 (Fireball) and 16, and every spell that shows something (`cast.pad` with
  `KF4_FX=spell=N`; 36, 61 and 76 showed nothing in 2026-11-11's window, so try longer).
- Creatures' own effects: the fire elementals (map 102), the fire genie (025), salamanders (016), watchers (022),
  as in `motion_compare.py`.
- Every page of the menu (`menus.pad`), the pause, a message box, the message dim, a save point.
- The map viewers (`mapview=1` to `5`) and a Symbol of Passage with the Green Wand (the map change's fade of its
  last frame).
- A teleport, a door to another map (its fade), the player's death and the death screen.
- The title, Press Start, New Game and the opening movie's end.
- The brightness option at 0x40 and 0xFF (`bright40`, `brightff`) and `--flicker-filter`.

**Settings:**
- The owner's (`KF4_READ_SETTINGS=1` with a copy of `~/.config/darker-reality` in `KF4_DATA_DIR`): frame rate 60,
  2x with native widescreen, FXAA, instant mouse look on.
- Frame rate 30 too (the game frames alone) and 60 (the pump's pictures between), since the inventory needs the
  layers in both kf4-pc's frames and the pump's.
- 1x and 4x for a few scenarios, to separate costs that scale with pixels from fixed ones.

For each scenario, keep:
- a GS capture of at least 4 vblanks (two game frames, so next-frame reads show);
- `kf4-gsreplay --list` of it;
- a `KF4_GPU_DETAIL=1` (and `=2` where needed) profile of a few seconds in a **real window** (see below);
- a picture, to tell what the scenario showed.

## How to run things here

- Build: `cmake --build out/linux-clang`. The disc: `/home/voicedrew/Documents/King's Field - The Ancient City
  (USA).iso` (not in `KF4_DISC`; pass `--disc`).
- **Timing in a real window, not headless** (CLAUDE.md, "Testing graphics"): leave out `--headless`, use
  `KF4_READ_SETTINGS=1` and a copy of the data folder in `KF4_DATA_DIR`, keep `--input ... --invincible --no-audio
  --saves none`. Headless is right for captures, `--list` and `--dump-frames`.
- The laptop runs on mains power with the performance profile for timings. On battery the GPU's and memory's clocks
  move (5-20%, `PC_OPTIMIZATION.md`).
- A mouse turn in a script: `KF4_DIAG_SPIN=RAD_PER_S`; it goes through instant mouse look as a real mouse does. Stick
  turns from a pad script need `KF4_TWIN_STICK_SCRIPTED=1` and don't trigger mouse look's extra draw.
- A pad script that gets into play: the first lines of `tests/twin_stick.pad` up to "the player has control"
  (control from about vblank 1500 to 1800); `--teleport VBLANK:MAP:PLACE` to go elsewhere.
- What a run did: check its `[stats]` lines (pictures between drawn, native draws not 0, the cut).
- `decomp/` is read-only. Don't edit game code for this step; if the game must be steered, `KF4_FX` is the place.

## The state of the tree you start from

On `main` at e05cd0ef, uncommitted:
- `port/gs/pc/pcr.cpp`: wait zones for the renderer's inbox (the sender waiting for room, for its picture, and for
  the renderer to be at most 2 vblanks behind). Built; not yet checked in the profiler window. Ask the owner whether
  to commit it.
- `docs/plans/`: this file, `camera-whip-slowdown.md`, `renderer-scene-not-gs.md`.

## Done when

- Every scenario above was run and its layers are in the inventory, or the inventory says why it couldn't be
  (a missing save, an effect that never showed).
- Every layer has its class, cost, readers and draw function, and every "must stay in memory" has the reader that
  forces it.
- The summary says what fast clears, dead-write removal and folding would each save per game frame and per picture
  between at the owner's settings, and which layers block each.
- The tool runs again from one command, and `tools/README.md` says how.
- The owner has the summary, and the decision between "targets first" and "fold first" for step 2 is made with it.

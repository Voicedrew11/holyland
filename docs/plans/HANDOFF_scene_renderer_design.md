# Handoff: settle the scene renderer's design

**Done 2026-11-19**: `scene-renderer-design.md`; the next step is `HANDOFF_scene_slice1.md`.

2026-11-19. For the next agent, on branch `measure/depth-by-layers` (or after it is merged). Read `CLAUDE.md` first,
then in this folder `renderer-scene-not-gs.md` (the direction and the owner's decision), and the four studies behind
it: `camera-whip-slowdown.md`, `fullscreen-layers-inventory.md`, `world-plain.md`, `depth-by-layers.md`.

## The mission

Write `docs/plans/scene-renderer-design.md`: the design of the renderer that replaces today's PC renderer, settled
enough that the next agents can build it in slices without re-deciding it. **Design, not the renderer**: read the
code, measure what a question needs (developers' switches and counters as the studies did, nothing a player gets),
prototype where a choice can't be made on paper, and lay out each choice with its evidence and a recommendation. The
owner decides the choices that change what players see; list them plainly.

The owner's standing wishes:
- **The scene renderer, not today's renderer.** No more work on speeding up the replay (`port/gs/pc/`); it stays the
  players' renderer, fixed where broken, until the scene renderer replaces it. Measurement modes in it are fine
  (they are how the studies were made).
- **Extremely fast on any GPU** (1080p on an integrated one), by general design, never per-GPU tuning or "lower your
  settings". The target at 2x on the Vega 7: the world 1-2 ms a game frame, a game frame a small fraction of
  today's 16-20 ms, a picture between less than a game frame.
- **Pictures stay exact**: as close to paraLLEl-GS as today's renderer or closer, measured, with every place where the
  design changes what "exact" means stated and decided by the owner on numbers.

## What is settled (don't re-open without new evidence)

- **Depth by the GS's precision** (`depth-by-layers.md`): the game's 3D uses a 16-bit Z buffer (PSMZ16S for every
  depth-tested draw counted); a `D16_UNORM` depth with each pixel's Z clamped by the depth clamp (the depth Z / zmax,
  the viewport's range 0 to 1) makes the GS's ties ties in the GPU's own test, the later draw winning by GEQUAL:
  early tests, no interlock, copy, planes or bias. 24- and 32-bit Z buffers (the menus') on D32F. The world at the
  GPU depth's cost; the tour closer to paraLLEl-GS than today's rule. Ties cannot be listed from the data: don't plan
  on knowing which surfaces lie on which.
- **The game's draw order is kept.** Reordering changes who wins the GS's ties (7% of fragments); front to back would
  save little (overdraw 3-6 fragments a pixel at 2x, mostly layers and effects). Batching consecutive draws of one
  state keeps the order.
- **The full-screen layers fold into the final pass** with the same arithmetic in the same order, dead writes left out,
  clears fast (`fullscreen-layers-inventory.md`: its classes and what blocks each).
- **Targets as images of their own** cost nothing extra (`world-plain.md`).
- **Pictures between from the scene**, not a replay; **the GPU never holds the game** (`camera-whip-slowdown.md`).

## The questions, in the order they constrain each other

1. **The frame as a scene.** What a game frame hands the renderer today, end to end: the GIF packets by PATH1-3, the
   native draws (`PORT_GS_REG_NATIVE_DRAW`: `port/include/port/render.h`'s `RdDraw`, its meshes and camera,
   `rd_fetch`), the frame marks (`frame_interp.c`'s 0xF0-0xF5, `RD_ND_FRAME`), uploads, local copies, read-backs,
   vblanks. Make a census of real frames (099's start, the pond, the palace, a creature fight, under water, a menu,
   the title): how many draws of each kind (`RD_KIND_*`, program, GIF 2D), states, textures, targets, uploads; what
   still comes as GIF (the HUD, the menus and text, the swap's clears, `GsDraw_*`, DATE's set-up, the passes:
   `pass_draw.hpp` makes them GIF primitives again, the trail strips). Then define the scene: its passes (one a target),
   its draws (mesh, instance record, material, GS state), its 2D, its layers, in the stream's order; and where it is
   built (the renderer's thread, as it meets the chain).
2. **Materials and the GS's memory.** A draw's material is the GS's registers and memory at its place in the chain
   (`PC_RENDERER.md`, "The first interface: P2, revised": `Map_DrawRecords` uploads map 0's textures, draws its
   records, then uploads map 1's over them). Today `pcr_textures.h`'s store (a page-ownership table over the uploads,
   textures decoded once, found by their uploads) resolves them in order, and textures read from drawn surfaces are
   copies. Decide how the scene renderer resolves and keeps materials (the store kept as the resolver? textures by
   content? the CLUTs?), what a frame uploads (counts and bytes), and what that means for pictures between, which
   draw a frame whose memory has since moved on.
3. **The targets.** Every target the game draws into and reads (`fullscreen-layers-inventory.md`, "Targets and the GS
   memory they share"): the double buffer, the reflection's 256 x 256 16-bit target (009, 014), the water and lava's
   128 x 128 targets, the sky's scrolling texture, the frame copied to 0x2BC0 (under water, the MP drain) and to 0x3540
   (the Dark Succubus), the menus' grab, the Z buffers and their aliases (`clear_depth_aliases`), read-backs. Which can
   be images of their own, which share memory in ways that matter, how each is cleared.
4. **Depth's exactness and DATE** (the owner's decision). Two candidates:
   - the `D16_UNORM` depth (measured: single pixels differ from the GS where its rounding and the GPU's fall either
     side of a half: 25 pixels in the 760-view tour against today's rule);
   - the GS's arithmetic in the shader: each pixel's whole Z exactly as the GS rounds it (no fraction, no tolerance,
     the plane from the transform that places the corners), written as the depth with a conservative bound so early
     tests still reject. Not built: measure it as a ceiling (a `KF4_PC_DEPTH=gsexact` mode in today's renderer, as
     `unorm` was, then the same tours, motion cases and `world_plain.py` runs) so the owner sees what the last pixels
     cost. Separate, too, how many of the 7,008 pixels UNORM still differs from paraLLEl-GS by are depth at all.
   And DATE (the entity pool's shadows, program 0x5A0): `D16_UNORM` has no stencil, and DATE by a copy costs 0.3-1.9
   ms a game frame at 2x. Count DATE's draws and pixels a frame; weigh reading the target's alpha in the shader
   (`VK_EXT_attachment_feedback_loop_layout`), a mask image of its own, or the shadows drawn another way.
5. **Draws: order, batching, the skinned models.** In the stream's order, how few calls can a frame be? Consecutive
   draws of one pipeline batched (instanced or multi-draw indirect, textures from a bindless table as the kept walks'
   runs already do: `pcr_canvas.cpp`'s `Runs`, merged runs), and how many state changes are left. Program 0x280 (the
   creatures, the map's skinned objects, the arm: about 890 batches a game frame at the pond, 2.4 ms at 1x with the
   GPU's depth): what varies between its draws (records, sphere passes, blends) and what batching it needs. The VU1
   maths stay on the GPU (`port/render/shaders/vu1_*_maths.h`, `vu1_record.hpp`); whether each corner's Z comes from
   them as the GS gets it (needed by either depth candidate).
6. **The 2D and the passes.** How the HUD, the menus, text and the screen passes are drawn (as GIF primitives, as
   today's canvas does, or as a 2D layer of the scene), and which full-screen layers fold into the final pass: each
   with its arithmetic, and the inventory's blockers (the frame before blended in keeps the last picture; tints
   under the HUD fold only at their place; the alpha copied to 0x2BC0).
7. **Pictures between.** A picture between drawn from the frame's scene at its time: the instances moved
   (`port/render/part_moves.hpp`, the records' `VU1_MOVE_*`), the camera, the 2D and passes (`gif_moves.h`,
   `pass_draw.hpp`'s `PassMoves`), the trails, the effects; what today's kept walks refuse
   (`pcr_frame_walk.h`'s `FrameWalkRefused`) and what the scene does there; what it needs of the frame's targets
   (none of the pump's surface copies, 2.4-3 ms a picture between today). Instant mouse look as a reprojection of the
   finished picture with a margin (`camera-whip-slowdown.md`, problem 2): the margin a fast whip needs, the parallax.
8. **Scheduling and present.** Pictures planned by measured cost so that the GPU never holds the game
   (`camera-whip-slowdown.md`, problem 1, including why today's cut left the game at 21 frames a second with no
   pictures between); the threads (today: kf4-pc and the pump); the final picture (scaling, sharpening, FXAA/SMAA,
   about 2.7 ms a vblank at 2x) as one pass with the folded layers; a vblank that shows the same picture not made
   again.
9. **Beside today's renderer, and how it is judged.** A `--renderer scene` beside `pc` and `gs` (`port/platform/gs_api.h`,
   `gs.c`), the same presenter, settings, overlay and save states; what it needs from Granite and the device (aim:
   nothing an integrated GPU of the last ten years lacks: no interlock, no 64-bit integers; check descriptor
   indexing, multi-draw indirect, dynamic rendering, D16 with the depth clamp on Intel and NVIDIA). How its pictures
   are checked: map tours and motion cases against paraLLEl-GS (`tour_compare.py`, `motion_compare.py`), the GPU
   time in a real window (`world_plain.py`, `layer_inventory.py --steps live`). Note that GS captures (`.kf4gs`,
   `kf4-gsreplay`) hold GIF packets, not native draws: whether captures should record the native draws too, so a
   scene can be replayed without playing.
10. **The slices.** The order to build it in, each a renderer someone can run, with what it must show to pass:
    for example game frames at 30 first (the world, the 2D, the targets, D16 depth, the folded layers), then pictures
    between, then batching and the skinned models, then the scheduling and present; and when today's renderer can go.

## The deliverable

`docs/plans/scene-renderer-design.md`:
- the frame's census and the scene's definition (data, not prose alone: counts at the scenes above);
- each question answered: the options, the evidence (measured where it could be), the recommendation;
- the owner's decisions, each with its numbers (depth's exactness first);
- the slices, each with its acceptance (pictures, GPU ms) and its risks;
- what it needs of Granite and the device, and what of today's code it reuses (`vu1_*_maths.h`, `vu1_record`,
  `part_moves`, `pass_draw`, the texture store, `pcr_blend.h`, the presenter, the anti-aliasing passes).

Update `renderer-scene-not-gs.md`'s "For the design" with what is settled, and write the handoff for the first slice.

## Where to start

- The inputs: `port/include/port/render.h` (the renderer core's interface: meshes, cameras, draws, the passes),
  `port/render/` (`render.cpp`, `vu1_record.*`, `part_moves.*`, `pass_draw.*`, `ripple.*`, `shaders/`),
  `game/native_draw/` (each kind: the map's records, objects, entities, the arm, the sky, the water, the effects, the
  shadows, the passes), `port/platform/frame_interp.c` (the frame's marks, pictures between on the game's side).
  `docs/port/NATIVE_DRAW.md` and `NATIVE_DRAW_P8.md`, which comments cite, were removed (40a969d5): read them from git
  history if a comment's reference matters.
- Today's renderer, to learn from and borrow (not to speed up): `port/gs/pc/pcr.cpp` (the threads, the inbox, the pump),
  `pcr_native.cpp` (native draws to batches), `pcr_canvas.cpp` (surfaces, states, clears, DATE, blends, the kept walks'
  runs), `pcr_textures.*`, `pcr_between.*`, `pcr_frame_walk.h`, `pcr_dead_writes.h`, `pcr_gpu_time.*`, and
  `docs/port/PC_RENDERER.md` (its history: P2's survey, P7's world off the GS's batches with an invariant vertex stage,
  what failed and why).
- The studies' tools and data: `layer_inventory.py` (scenarios, captures, live GPU time), `world_plain.py`,
  `depth_ties.py` and the counters (`KF4_PC_TIES`, `KF4_PC_OVERDRAW`), `map_tour.py`, `tour_compare.py`,
  `motion_compare.py`; the raw runs in the owner's `~/Documents/darker-reality-measurements/` (its README; the
  inventory's captures were made with native draw off).

## How to run things here

- Build: `cmake --build out/linux-clang`. Disc: `/home/voicedrew/Documents/King's Field - The Ancient City (USA).iso`
  (pass `--disc`). Tests: `ctest --test-dir out/linux-clang`. A Python with numpy for the comparisons: the venv in
  `~/Documents/darker-reality-measurements/2026-11-19-depth-by-layers/venv`.
- Scripted runs read no settings: pass `--native-draw on` (else the native draws are off) and, for the studies'
  comparisons, `--frame-rate 30` and `--native-scale 1 --no-native-widescreen` (tours) or the owner's settings through
  `KF4_READ_SETTINGS=1` (real windows).
- Timing in a **real window** with the owner's settings (`CLAUDE.md`, "Testing graphics"), on mains power, the
  balanced profile, nothing else on the GPU, no building while it runs; say when windows will open.
- **Output on disk, not in `/tmp`** (a RAM disk on the owner's laptop): a dated folder under
  `~/Documents/darker-reality-measurements/`, added to its README.
- Don't rebuild while a tour or motion run is going: their later runs would use the new binary.
- `decomp/` is read-only. Commit on a branch; ask the owner before pushing.

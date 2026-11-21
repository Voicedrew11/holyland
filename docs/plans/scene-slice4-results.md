# The scene renderer, slice 4: results

Slice 4 of `scene-renderer-design.md` ("The slices"): pictures between drawn from the scene's records at their time, and
mouse look from the scene with the late turn reprojected. On `scene/slice4` from `scene/slice3c` (PR #190).
Measurements: `~/Documents/darker-reality-measurements/2026-11-20-scene-slice4-4a/`.

## 4a. The bound, before building (in progress, 2026-11-20)

### The arithmetic

A picture between from the scene draws the frame's world and 2D again at the picture's camera; the per-vblank picture
(scanout, anti-aliasing, scaling) is paid whether a vblank shows a game frame or a picture between. From slice 3b's
`world_plain.py` (2x, real window, Vega 7, the detail off), ms a game frame at 30:

| | world | 2D | copies | scanout | all | half of all | world + 2D |
|---|---|---|---|---|---|---|---|
| 099 | 3.82 | 0.53 | 0.54 | 5.54 | 10.43 | 5.2 | 4.35 |
| the pond | 7.39 | 0.13 | 0.12 | 5.45 | 13.12 | 6.6 | 7.52 |
| the palace | 6.69 | 0.34 | 0.10 | 5.59 | 12.73 | 6.4 | 7.03 |

So a picture between drawn again from the scene can meet "under half a game frame's GPU time" at 099, but not at
the pond or the palace (0.6-0.9 ms over): the world alone is 56-57% of a game frame there. Nothing in a redraw at a
new camera removes the world's own cost; that is slice 3f's study (where the world's GPU time goes).

### The test

`KF4_SCN_BETWEEN_BOUND=1` (developers', wrong pictures): the frame's scenes are kept as frame_interp.c takes the frame,
and each picture between records them all again, unmoved, into the same targets: an upper bound on a picture between
from the scene (it also remakes uploads, copies and the scrolling textures). `world_plain.py --variants
scene_nodetail,scene_bound_nodetail --rates 30,60`: a picture between's GPU time is the 60 run's less the 30 run's, a
game frame. `bound/world_plain.md`, two rounds, medians, ms a game frame (at 60 a game frame has one picture between):

| | all at 30 | all at 60, bound | a picture between (60 less 30) | of it the world | the 2D and copies | half a game frame |
|---|---|---|---|---|---|---|
| 099 | 10.52 | 15.19 | 4.67 | 3.90 | 0.80 | 5.26 |
| the pond | 13.09 | 21.58 | 8.49 | 7.56 | 0.28 | 6.55 |
| the palace | 12.81 | 21.37 | 8.56 | 7.42 | 0.62 | 6.41 |

- **A redraw costs what the first draw does**: the world drawn twice is twice the world to 0.1 ms (no warm cache, no
  cheaper second pass). Without the bound, 60 costs 0.1-0.4 ms more than 30 (a picture between shows the last picture).
- So "under half a game frame" holds at 099 and not at the pond or the palace, by the world's own cost; at the pond and
  the palace the picture's kinds grow 0.5 ms too (not yet explained).
- At 60, 2x, on the Vega 7 a game frame's two vblanks fit (about 13 and 8.5 ms against 16.7 each). At 240 they do not:
  a picture between alone is 8.5 ms where a picture has 4.2.

### What it means (the owner, 2026-11-20)

The owner expects very high frame rates on this integrated GPU: the bar is absolute (240: about 4 ms a picture, all
of it), not half a game frame. Every picture must draw the world again at its own camera, so the world's cost a draw
(7.4 ms at the pond at 2x, for a PS2 frame of 12,000-44,000 triangles) and the final pass's (about 2.75 ms a picture)
are what the frame rate is made of. Slice 4 is still needed (no restore, no third redraw, pictures between from the
scene), but the frame rate is slice 3f's to win, which moves up: after 4a, before 4b and slice 5, starting with cheap
tests that tell the costs apart (1x against 2x, anisotropy, a trivial fragment shader as the shader's bound, colour
compression and the images' layout).

### Found on the way

- Under the scene renderer, frame_interp.c's exact restore (`port_interp_end_vblank`) redrew the whole frame's packets
  after every frame's pictures between (684 of 684 frames in a 2,130-vblank run at 60). The scene renderer's pictures
  between will never draw into the GS's memory, so it needs no restore: `port_gs_between_leaves_memory()` turns it off
  under `--renderer scene` (`KF4_SCN_RESTORE=1`: as before).
- The frame is mostly handed over at its first picture between, a vblank after its packets: the shell now tells the
  drawing which stretch of packets is the frame's (`Drawing::frame_taken`: the open one, or the one closed at the last
  vblank).

## 4a. Pictures between from the scene

### What it does

- **The frame kept.** While frames are taken (and 8 vblanks after), the shell keeps the frame's stretch of packets as it
  does for the PC renderer's own walk (`pcr.cpp`'s `Intake`, now without a canvas too): the kicks numbered, the native
  draws' records put twice (the frame's and its pictures', `NativeBatch::picture_instance`), the frame's kicks and the
  trail's and the passes' native kicks made the frame's walked kicks (`Between::adopt_live_kicks`). The builder keeps the
  scenes it closes between two vblanks (a stretch), and when frame_interp.c hands the frame over
  (`Drawing::frame_taken`) takes the one that is the frame's: the stretch still open (the frame taken at its own vblank,
  the mouse turned) or the one closed at the last vblank (mostly: the frame taken at its first picture between).
- **Its corners name their kicks.** A GIF primitive whose kicks are numbered carries them in its corners as the canvas's
  do (`pcr_prim.vert` moves them at a picture's time; outside a picture the shader draws them where they are, so the
  frame is unchanged). A triangle crossing the Z buffer's range is cut for the frame as before; its pieces are marked
  the frame's only and the whole triangle, of its kicks, the pictures' only (`pcr_prim.vert`'s `PCR_K_FRAME_ONLY` and
  `PCR_K_PICTURE_ONLY`, which the PC renderer never sets).
- **A picture between is the kept scenes recorded again** (`scn_build.cpp`'s `picture_between`, `scn_draw.h`'s
  `PictureDraw`): the picture's buffers bound where the shaders read a picture's (its camera, t, the kicks and their
  moves: `pcr::Between`'s), each native draw by its picture's record, and every image the frame's steps write (its
  targets, the target copies, the first-draw image, the history) drawn into a **stand-in** of its own: the frame's buffer
  and Z copied from the live image around the rows the frame clears, the first-draw image cleared, a small target first
  drawn without a clear copied whole. Nothing of the GS's memory is drawn into, so frame_interp.c's exact restore is
  not made under the scene renderer (`port_gs_between_leaves_memory`). The stand-ins are pooled by size and format.
- **The picture that shows it** reads the frame's buffer from its stand-in (`scanout_picture`), with the frame's kept
  layers (the brightness, the fills after the HUD) composed over it from the stand-in as a game frame's are.
- `tools/motion_compare.py --rate N --renderer pc,scene`: paraLLEl-GS and each renderer at N, every picture (the
  pictures between too) paired with paraLLEl-GS's, whose pictures between are frame_interp.c's stream drawn by the GS:
  the reference. `report_N.md` sums each case's pictures between apart from its game frames.
- Developers': `KF4_SCN_BETWEEN_LOG=1` (the first frames' stand-ins), `KF4_SCN_BETWEEN_BOUND=1` (the bound above),
  `KF4_SCN_RESTORE=1` (the exact restore as before).

### Against paraLLEl-GS at 60

`motion_compare.py --rate 60 --renderer pc,scene --profiles 1x --pc-extra "--native-draw on"` (`motion60/`): the 15
cases, paraLLEl-GS at 60 drawing frame_interp.c's stream as the reference, each picture paired. At 60 a vblank's own
picture is the picture between, so 410 of the 745 pictures are pictures between. Sums over the cases:

| | pictures between: over 24 levels | mean | game frames: over 24 levels | mean |
|---|---|---|---|---|
| the PC renderer | 208.78% | 747.39 | 50.64% | 414.54 |
| the scene renderer | 208.41% | 747.65 | 50.37% | 413.63 |

**No further than today's pictures between** (the criterion): lower in pixels over 24 levels, level in the mean (0.03%
more, the title's 0.8 and combat's 0.26; spell 3, spell 16, underwater and drain a little less). Salamanders are
closer than the PC renderer's in both (0.00% against 0.24%). Case by case: `motion60/report_60.md`.

At 240 paraLLEl-GS cannot be the reference for the pictures at phases: only the PC renderer's shell draws extra pictures
(gs.c's present_extra), so a `--renderer gs` run dumps each vblank's own picture alone, the same pictures as at 60. The
pictures between at 240 are judged as the tool always judged them: flash_find against their game frames, and the game
frames at 240 against those at 30, both renderers (below).

### At 240

`motion_compare.py --renderer scene` and `--renderer pc` (`flash240_scene/`, `flash240_pc/`, 1x, native draw on): the
pictures between at 240 (four a vblank) checked by flash_find against their game frames, and the game frames at 240
against the same vblanks at 30. **The two renderers alike in every case**: flagged pictures between the same (first kill
10 of 288, spell 3 27 of 240, the PC renderer's same pictures; none elsewhere), no game frame at 240 differing from 30's
in either. At 30 against paraLLEl-GS the same too (salamanders 0.00% against the PC renderer's 0.01%).

### The GPU's time (real window, the owner's settings, 2x, Vega 7)

`world_plain.py --variants scene_nodetail --rates 30,60 --rounds 2` (`cost4a/`), ms a game frame (at 60 a game frame
has one picture between), medians:

| | all at 30 | all at 60 | a picture between (60 less 30) | the bound's | of it 0x000; 0x280 and 0x700 | the stand-ins' copies |
|---|---|---|---|---|---|---|
| 099 | 10.45 | 16.65 | 6.20 | 4.67 | 4.18; 0.72 | 0.95 |
| the pond | 13.13 | 27.26 | 14.13 | 8.49 | 4.94; 7.05 | 1.82 |
| the palace | 12.76 | 26.52 | 13.76 | 8.56 | 4.12; 5.99 | 1.21 |

**A picture between costs more than the frame's own draw**, far over the bound: the moved world is dearer than the
unmoved one (at the pond 0x280's pictures 4.7 ms against the frame's 1.9, 0x000's 4.9 against 3.4), since the
pictures' moves (`vu1_*_maths.h`'s `moved()`: at 0x280 a slerp, sines and cosines, a corner) are made in a vertex stage
that is the world's cost already (3f below), once a triangle corner; and the stand-ins were copied around the frame's
rows (the other frame buffer and its Z, which nothing read): 1.8 ms at the pond. That copy is now made only where an op
of the frame copies from the image (pictures unchanged: underwater, combat, first kill, the menus byte for byte the
same scores). Measured again (`cost4a_trim/`, same run otherwise), **it saves next to nothing**:

| | all at 30 | all at 60 | a picture between | before the trim | the copies at 60 |
|---|---|---|---|---|---|
| 099 | 10.42 | 16.63 | 6.21 | 6.20 | 1.42 |
| the pond | 13.18 | 27.00 | 13.82 | 14.13 | 1.75 |
| the palace | 12.76 | 26.31 | 13.55 | 13.76 | 1.30 |

Still 2.7 stand-in copies a picture (2176 for 796 pictures at the pond): the copies left are the whole images an op of
the frame reads and the shown picture's copy out of its stand-in, not the rows around the frame. Copies are not where
a picture between's time is; the moved world is (3f below).

## 3f's cheap tests: what the world is made of

`world_plain.py` (`bounds3f/`), 30, two rounds, ms a game frame. `KF4_SCN_TINY=1`: every plain draw's scissor one pixel
(the vertex stage, the draws and the rasteriser's set-up left, no pixels); `KF4_SCN_FLAT=1`: every plain draw shaded
one colour (the GS's pixel arithmetic gone; the depth test, the blend and the rasterising kept):

| | world 2x | no pixels | one colour | world 1x | anisotropy 1x | colour compression off | HiZ off | the picture and present |
|---|---|---|---|---|---|---|---|---|
| 099 | 3.81 | 1.20 | 1.62 | 2.22 | 3.55 | 3.82 | 3.92 | 5.53 (2.75 a picture) |
| the pond | 7.39 | 4.60 | 5.59 | 5.27 | 7.23 | 7.52 | 7.55 | 5.45 |
| the palace | 6.66 | 4.17 | 3.32 | 5.54 | 6.65 | 6.66 | 6.72 | 5.62 |

- **At the pond and the palace most of the world is not pixels**: 4.6 and 4.2 ms with none drawn, nearly all of it the
  native programs' vertex stage (0x280 2.15 ms at the palace with no pixels). Each triangle corner is a vertex shader
  invocation that runs VU1's maths for its mesh vertex whole (`pcr_nd*.vert`: a triangle the arena's slots of its
  corners, `gl_VertexIndex % 3`, a draw without indices: no vertex reused), so a mesh vertex's maths is made once for
  each triangle corner that uses it, about three times a pass (0x280's sphere pass again). In a picture between
  (`ND_C_MOVING`) each corner's invocation also places all three of its triangle's corners moved (`whole_place` of
  `tri.x`, `tri.y` and `tri.z`, for `whole_triangle.h`): up to nine times a pass. This is why a picture between's world
  costs more than the frame's.
- **The GS's pixel arithmetic in the shader is the rest**: 2.2 / 1.8 / 3.3 ms (the world less one colour's).
- **Texturing, memory and compression are not it**: anisotropy, RADV's colour compression and HiZ change nothing.
- **The final pass is 2.2-2.75 ms a picture whatever the world costs** (5.5 a game frame at 2x, 4.3 at 1x).

So for a high frame rate on this GPU (240: about 4 ms a picture, all of it), three structural costs, none of them
tuning: (1) each mesh vertex's maths (and a picture's move) made once a draw, not once a triangle corner; (2) the final
pass (scanout, anti-aliasing, scaling and sharpening) a fraction of its 2.75 ms; (3) the GS's pixel in the shader
lighter where its parts are not used. (1) is the largest and the one pictures between multiply.

## Vertices once (3f's first part, before 4b; branch `scene/vertices-once`)

0x000's and 0x280's native draws drawn from each mesh vertex worked out once a draw. A compute pass before the scene's
steps (`scn_draw.cpp`'s `prepare_placing` and `place_vertices`, `port/gs/scene/shaders/scn_place_000.comp` and
`scn_place_280.comp`) runs VU1's maths (`vu1_000_maths.h`, `vu1_280_maths.h`, unchanged) for each vertex of each job, a
draw's run of the arena's vertices with the instance it is drawn with (`NativeBatch::first_vertex`, `vertex_count`; a
run met again in another pass, as DATE's shadows', is one job), into 80 bytes a vertex (`port/render/shaders/
scn_placed.h`): its place, Z, Q, S T Q, fog, colour, the mirror's cut, and in a picture between its moved place, drop
and whole corner. `pcr_nd000.vert` and `pcr_nd280.vert` made with `SCN_PLACED` read their corners from it (the
command's firstInstance is the job). `KF4_SCN_PLACED=0` draws them as before. The PC renderer's shaders are unchanged.

### The bound, before building (`2026-11-20-scene-vertices-once/bound`, `empty`)

Real window, 2x, no pixels (`KF4_SCN_TINY=1`), ms a game frame:

| | 0x000 | ... a third of its triangles (`KF4_SCN_THIRD=1`) | ... no maths (`KF4_SCN_ND_EMPTY=1`) | 0x280 | ... a third | ... no maths |
|---|---|---|---|---|---|---|
| the pond, 30 | 2.15 | 1.11 | 0.19 | 1.90 | 1.39 | 0.35 |
| the palace, 30 | 1.96 | 0.94 | 0.14 | 2.15 | 1.90 | 0.19 |
| the palace, 60 | 6.31 | 3.01 | 0.42 | 7.46 | 6.72 | 0.40 |

The maths is over 90% of both programs' vertex stage; the draws themselves are cheap (0x280's 260 a scene about 0.7
us each with none). 0x000's time follows its corners; 0x280's hardly (a third of its triangles left 6.72 of 7.46): its
small draws, a wave or so each, one after another, each a wave's whole latency, and the shader (as RADV's statistics
showed later, `RADV_DEBUG=shaderstats`) 248 registers, one wave a SIMD and 52 KB of scratch a wave.

### Found building it

The first placing was slower than the shaders it stood in for (0x280 at the palace 4.5 ms against 2.6). The compute
shaders copied the draw's record (`Draw d`, about 1.4 KB) into a variable, and its arrays indexed at run time
(`d.vertex[s]`, `d.point_pos[ps + l]`) put the copy in scratch memory: 52 and 72 KB a wave. Read from its buffer where
used (`ND_D_DIRECT`: `d` a macro for `draws[d_at]`), the placing has no scratch, 48 and 64 registers, 4 and 5 waves a
SIMD; the placed vertex shaders are about 450 instructions with 32 to 36 registers.

### The pictures

Headless, the scene renderer at 60 with native draws on, `KF4_SCN_PLACED=1` against `=0`, nine motion cases (menus,
combat, first kill, spell 3, salamanders, watchers, under water, drain, fire elementals): 507 pictures, game frames and
pictures between, **byte for byte the same**; against paraLLEl-GS the same scores as 4a's. A fast turn at 240 (about
46 degrees a game frame, `look` lines in a pad script: the pictures between draw the triangles VU1 left out whole): of
72 pictures, 8 pictures between differ in 1 to 3 pixels by 1 level of 255, the whole corners (`whole_place`, the
PC's own maths, not VU1's exact arithmetic) made in the compute stage, whose float contraction differs from the vertex
stage's: invisible. The validation layer with synchronization validation (the fast turn at 60, combat, fire
elementals; `validation/`, `validation_s4/`): no hazard from the placing; the two usage errors it found were 4a's (a
stand-in cleared without `TRANSFER_DST`, an op's copy copied from without `TRANSFER_SRC`), fixed on `scene/slice4`.

### The GPU's time (real window, the owner's settings, 2x; `cost2/`)

ms a game frame, medians of two rounds; a picture between is the 60 run less the 30:

| | all at 30 | all at 60 | a picture between | before (4a, trimmed) | half a game frame | world at 30 (before) | 0x280 at 30 (before) |
|---|---|---|---|---|---|---|---|
| 099 | 9.97 | 14.74 | **4.77** | 6.21 | 4.99 | 3.66 (3.81) | 0.10 (0.26) |
| the pond | 11.05 | 18.02 | **6.97** | 13.82 | 5.53 | 5.24 (7.38) | 0.59 (1.93) |
| the palace | 10.66 | 16.83 | **6.17** | 13.55 | 5.33 | 4.53 (6.66) | 1.28 (2.61) |

A picture between halved at the pond and the palace and is under half a game frame at 099. What is left in one, at the
palace: 0x000 3.24 (now its pixels: 0.98 with none drawn), 0x280 1.69, the 2D 0.35, the stand-ins' copies 0.83; at the
pond 0x700 (the effects, not placed: its shader one of the 244-register ones) about 2.1 ms a game frame and as much
again a picture, three quarters of it pixels. The final pass, 5.5 ms a game frame, is untouched.

Next on the same way: 0x700 placed (the pond), then the final pass and the GS's pixel in the shader (3f's (2) and (3)).

### 0x700 next: how to do it well

The effects (`pcr_nd700.vert`, `vu1_700_maths.h`; the pond's spray and the fire creatures) are about 2.1 ms a game
frame at the pond and as much again a picture between, mostly pixels (below). Their vertex shader is one
of the two with 244-248 registers and a wave a SIMD (`RADV_DEBUG=shaderstats`, `MESA_SHADER_CACHE_DISABLE=true`).
What 0x000's and 0x280's taught, in order:

1. **Bound it first** (an hour, no building): extend `KF4_SCN_ND_EMPTY` to 0x700 (`pcr_nd700.vert` made with
   `SCN_ND_EMPTY`, `vs_rec[nd700]`) and `world_plain.py`'s `scene_tiny_empty_nodetail`, at the pond, 30 and 60.
   Already known (`cost2/`, the "0x280, 0x700" column less 0x280's): 0x700 at the pond is 2.08 ms a game frame at 30,
   **0.51 of it with no pixels**; a picture between's 2.15, 0.67 of it with none. So placing can win at most about
   0.5 ms a frame and 0.6 a picture: most of 0x700 is its pixels (blended effects over much of the screen), which
   is 3f's (3), the GS's pixel in the shader. Placing 0x700 alone does not bring the pond under half a game frame.
2. **Read the record where used, everywhere.** `vu1_700_maths.h` has the same `Draw d;` copy as 0x000's and 0x280's
   had; the placing shader must take `ND_D_DIRECT` (`#define d draws[d_at]`) from the start. Check its statistics
   before timing it: no scratch, under ~64 registers. It may be worth trying `ND_D_DIRECT` in the unplaced vertex
   shaders too (0x700's and the ripple's) before placing them at all: the 244 registers may be that copy.
3. **The batch's vertices** (`draw_700` in `pcr_native.cpp`): set `nb.first_vertex = base + b.first`,
   `nb.vertex_count = b.count` (one slot a vertex; `draw_left_out` already does). 0x700's loop transforms a "vertex n"
   past the strip (`RdBatch::past`), but no triangle corner reads it: the job is the strip's `count`.
4. **What 0x700's corners need from a vertex differs from 0x000's** (`pcr_nd700.vert`), so read it line by line:
   - the position's own w (`mesh.q[at * 4].w`, 16 for the effects): the mirror cut is `cut / pw` in a whole triangle and
     `cut * q * f` on the screen, and a whole triangle's Q is `1 / (c.w * pw)`. Keep `pw` in the placed vertex (a pad
     word of `Placed`);
   - the drop in a picture between looks at the kick and the two vertices before it (`kick - i`, `i < 3`) with no check
     of the batch's start (0x280's checks its number in the batch, 0x000's `befores`): keep that exactly;
   - no ADC clip test of its own (VU1's count decides the whole strip on the CPU), no wave UVs, no far plane;
   - `lit_colour` is the model's one colour by the lights (no point lights, no `colour_u` walk).
   One `Placed` layout fits; `place_vertices` takes a third program (native slot 2: set 0's bindings 9 and 10), and
   `prepare_placing`'s `slot > 1` test becomes `slot > 2`.
5. **The pictures**: byte for byte against `KF4_SCN_PLACED=0` in the cases with effects (fire elementals, salamanders,
   spell 3, spell 16, fire genie, the pond), headless at 60 with `--native-draw on` (without it no native draw is drawn
   and the check checks nothing); the fast turn (`look` lines, `2026-11-20-scene-vertices-once/turn/turn.pad`) at 240,
   where only the whole corners may differ, by a level.
6. **Then time it** in a real window (`world_plain.py`, 2x, 30 and 60, as drawn and with no pixels).

The same holds for the ripple (`pcr_ndripple.vert`), small today.

## What is left of slice 4

- 4a proper: the kept frame scene drawn again at t into images of its own (stand-ins for every target it writes, the
  target copies and the first-draw image too), the native draws by their picture records (`NativeDraws::
  begin_frame_kept`, `picture_instance`), the GIF corners numbered by their kicks (as `pcr_canvas.cpp`'s
  `add_primitive`), the picture's buffers (`pcr::Between`), the next scanout reading the stand-in frame with the kept
  layers; `motion_compare.py` taught a rate (60, 240) with paraLLEl-GS run at it too.
- 4b: mouse look from the scene, the late turns measured and the margin sized from them, the reprojection (below,
  "4b").

### What removing the third redraw takes (found building 4a)

At 60 with the mouse turning, the pictures shown are all pictures between: at a game frame's own vblank frame_interp.c
asks for a picture of the new frame at t 0 turned to the mouse (the frame before's camera and the lead), then one at
t 0.5. With 4a both are drawn from the scene, so mouse look already works and is "drawn from the scene". But the
frame's own scene is still drawn into the GS's targets when it closes, and with the mouse turning nothing ever shows
that draw: it is the third. It cannot simply be left out, since the GS's memory must be right for whatever reads it
later (the next frame's scrolling targets, the frame before blended in, the map change's last frame, save states,
read-backs), and at 30 or with the mouse still it is the picture shown.

The way that fits the builder: **the frame's passes into its own buffer held back** as slice 2 holds clears back:
drawn only when something reads the frame buffer's pixels (a picture of it, a texture or copy of it, a read-back, a
save state) and dropped as dead when the next frame's clear of that buffer comes first. The other targets (scrolling
textures, the reflection, copies) are drawn as now. A held pass drawn late uses a Z of its own (it clears its Z first),
so it cannot disturb the Z of the frame drawn since. With the mouse turning, the frame buffer is never read before it is
cleared again: two world draws a game frame at 60 in place of three. To be bounded first by a switch that drops the
frame's passes when a mouse picture was drawn for it (wrong only where the frame is read later).

## 4b. Mouse look from the scene (branch `scene/slice4b`, 2026-11-20)

Measurements: `~/Documents/darker-reality-measurements/2026-11-20-scene-slice4b/`.

Mouse look was already drawn from the scene by 4a: frame_interp.c's turned pictures (the frame at t 0 turned to the
mouse at its own vblank, then the pictures between) are pictures between, each turned again to the hand as it is drawn
(`pcr.cpp`'s `turn_to_hand`) and drawn from the kept scene. What 4b adds: the third world draw gone, and the late turn
measured.

### The third redraw: the bound (`bound/`)

`KF4_SCN_DROP_FRAME=1` (developers', wrong pictures): the frame's own passes into its buffer (and the shadows'
first-draw passes) left out where the frame is taken at its own vblank. `world_plain.py --scenes "start spin,pond
spin,palace spin" --variants scene_nodetail,scene_drop_nodetail --rates 30,60 --scales 2 --rounds 2` (new scenarios:
the pond and the palace standing at their teleport places, KF4_DIAG_SPIN=6: a whole turn's views, not the views of
"pond" and "palace"), ms a game frame, medians:

| | world at 30 | world at 60 | left out | all at 60 | left out | saved |
|---|---|---|---|---|---|---|
| 099, the spin | 3.14 | 9.31 | 6.15 | 19.30 | 16.31 | 2.99 |
| the pond, the spin | 3.07 | 9.39 | 7.22 | 17.65 | 16.15 | 1.50 |
| the palace, the spin | 3.18 | 10.17 | 6.88 | 19.52 | 16.51 | 3.01 |

At 60 with the mouse turning the world is drawn three times a game frame (9.3 against 3.1), and leaving out the frame's
own passes takes one of them away (at the pond 2.2 of 3.1: the reflection's view and the scrolling targets are not the
frame's buffer and stay).


### The frame held back (`scn_build.cpp`'s `HeldFrame`; `KF4_SCN_HOLD_FRAME=0`: as before)

As slice 2 holds clears back: where frame_interp.c takes the frame at its own vblank (the mouse turned), the frame's
steps into its buffer are not drawn as its scene closes but held, and recorded late, before whatever then comes, only
where something uses what they drew:

- **What is held**: the passes into the frame's buffer and the shadows' first-draw passes for them (`plan_hold`), and
  every step after them that reads what a held step writes or writes it again. None held where one of those writes
  another of the GS's targets (counted: the frame copied to 0x2BC0 under water is not, it copies from the frame
  before), or uses a second Z.
- **Renames** (`rename_reads`): the frame reads effects drawn between its own passes. At 099 a 128x160 target (the
  clouds) is drawn, copied into a 128x128 copy the frame reads, drawn again from that copy and copied again, three
  times a frame. Holding every step that writes the copy again would hold the target too. Instead, where a step drawn
  now writes again what held steps read, the copy is copied just before the first of those readers (an op of the
  scene, drawn now and by the pictures), and the held reads point at that copy. (First built before the writer, not the
  reader: right for the late draw, but the pictures between draw the whole scene in order and read the copy before it
  was made, a level or three off in the clouds. Found by the byte compare below.)
- **Dropped** where a clear covers what the held steps drew of the frame's buffer (`held_use`, called by `written_over`
  and `touch`, every use of an image the builder sees). **Drawn late** where a draw, an op, a read-back or a picture of
  the frame meets it, where their Z is read before it is cleared, at a save state's memory (`vram_image`) and before a
  scale change. Drawn late, they draw what they would have: into the live images, with a first-draw image of their own
  (cleared), a Z of their own where the live one was written over since (as is: their first use of it clears it), and
  any other image they read copied as something first writes it (`snapshot`).
- Developers': `KF4_SCN_HOLD_LOG=1` (the first frames: what is held, renamed, copied, or why not),
  `KF4_INTERP_T0=1` (frame_interp.c: the frame's own vblank drawn as a picture between at t 0 with no mouse, so that
  frames are held under a pad script that repeats; the mouse's turns go by the wall clock).

### Exact: byte for byte (`hold_all/`, `check/`)

`tools/hold_compare.py` (new): every case of `motion_compare.py` played by the scene renderer with the frame held and
not (`KF4_SCN_HOLD_FRAME=1` and `0`, both with `KF4_INTERP_T0=1`), each picture compared byte for byte.

**0 of 6,556 pictures differ**: the 13 cases at 1x and at 2x widescreen (`--native-draw on`), at 60 and at 240. In
every case but two, frames are held and nearly all dropped as dead (combat 862 held, 854 dead, 6 drawn late; spell 3
635, 631, 2; the title 475, 472, 1; a frame or two still held at the end). Under water and the drain hold none: their
frame is copied into 0x2BC0 between its passes (another of the GS's targets), so it is drawn as its scene closes, as
before. The late draws were also made many times over (`check/`: the walk at 2x widescreen with a save state every 37
vblanks, 25 held frames drawn late for the states' memory): 0 of 150 pictures differ.

### The GPU's time (`held/`: real window, the owner's settings, 2x, Vega 7)

`world_plain.py --scenes "start spin,pond spin,palace spin" --variants
scene_unheld_nodetail,scene_nodetail,scene_drop_nodetail --rates 30,60 --scales 2 --rounds 2`, ms a game frame,
medians (`scene_unheld_nodetail`: `KF4_SCN_HOLD_FRAME=0`, as before):

| at 60, the spin | world: as before | held | the bound | all: as before | held | the bound | saved |
|---|---|---|---|---|---|---|---|
| 099 | 9.34 | 6.15 | 6.12 | 19.36 | 16.50 | 16.33 | 2.86 |
| the pond | 9.26 | 6.94 | 6.90 | 17.53 | 15.37 | 15.14 | 2.16 |
| the palace | 10.25 | 6.84 | 6.78 | 19.53 | 16.60 | 16.29 | 2.93 |

**The third world draw is gone**: held, within 0.1-0.3 ms of the bound (the copies the renames and the held frames
make: 0.2-0.4 ms more "copies" than the bound's). At 30 nothing changes (10.8 / 10.1 / 10.8, the frame is never taken
at its own vblank).

### The late turn (`late/`)

`KF4_LATE_TURN_LOG=FILE` (developers', `present_vk.cpp`): each picture that shows a picture between carries its mouse
turn to the presenter (`GsnScanout::turn`: the hand it was turned to, when, and when it is to be shown), and as the
presenter records the picture it logs what the hand has moved since: the turn a reprojection there would add.
`bin/late_turn.sh` (the spin at 099, the owner's settings in a real window, KF4_DIAG_SPIN=6) and `bin/late_turn.py`:

| at 60 (the owner's) | median | p90 | p99 |
|---|---|---|---|
| the picture's turn to the presenter's recording of it | 12.1 ms | 15.9 ms | 16.0 ms |
| ... to the time it is shown | 16.1 ms | 19.9 ms | 20.0 ms |
| the late turn at 6 rad/s | 92 mrad (5.3 degrees) | 95 mrad | 95 mrad |
| ... in the GS's pixels (sx 384: 80 degrees across 640) | 35.5 (5.5% of the width) | 36.3 | 36.5 |

At 240 the GPU does not keep up with the spin (16-20 ms a game frame at 60 already): pictures are shown up to a
quarter of a second after their time and the late turn means nothing but that backlog (slice 5's scheduling).

**The margin, sized: not small.** The design took the late turn to be the last few milliseconds' turn, a degree or two
(decision 3). It is 12 ms at 60, 5.3 degrees at 6 rad/s and four times that in a flick of 20 rad/s: a margin of 6% or
more of the width a side, and more pixels than the design wanted to draw. And it is not the drawing's time: a picture is
drawn as soon as it is asked for, a vblank before it is shown, then waits. So:

- **The reprojection is not built.** Most of the late turn is slice 5's to take away: each picture begun no sooner
  than its predicted cost before it is shown (as `pcr_late.h`'s `begin_at_s` does for the PC renderer's pump at an
  unlocked rate), the hand then a few milliseconds old. What remains after that is the turn a reprojection could
  cover, sized again then.
- A reprojection of the picture as it is would move the HUD with the world (the HUD is drawn into the frame's buffer):
  at 5 degrees, the orb by about a hundred pixels. It needs the HUD in an image of its own (decision 4's "only if a
  later measurement says it is worth it"): this is that measurement, for after slice 5.

## After 4b

Nothing of 4a and 4b but the reprojection, moved behind slice 5 (above). Slice 5 starts from: pictures drawn just in
time (the late turn), the held frame's two world draws a game frame at 60, and the world's own cost (3f).

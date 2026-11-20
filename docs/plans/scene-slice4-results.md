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

## What is left of slice 4

- 4a proper: the kept frame scene drawn again at t into images of its own (stand-ins for every target it writes, the
  target copies and the first-draw image too), the native draws by their picture records (`NativeDraws::
  begin_frame_kept`, `picture_instance`), the GIF corners numbered by their kicks (as `pcr_canvas.cpp`'s
  `add_primitive`), the picture's buffers (`pcr::Between`), the next scanout reading the stand-in frame with the kept
  layers; `motion_compare.py` taught a rate (60, 240) with paraLLEl-GS run at it too.
- 4b: mouse look from the scene, the late turns measured and the margin sized from them, the reprojection.

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

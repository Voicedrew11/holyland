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
game frame. Results: `bound/world_plain.md` (to be read into this section).

### Found on the way

- Under the scene renderer, frame_interp.c's exact restore (`port_interp_end_vblank`) redrew the whole frame's packets
  after every frame's pictures between (684 of 684 frames in a 2,130-vblank run at 60). The scene renderer's pictures
  between will never draw into the GS's memory, so it needs no restore: `port_gs_between_leaves_memory()` turns it off
  under `--renderer scene` (`KF4_SCN_RESTORE=1`: as before).
- The frame is mostly handed over at its first picture between, a vblank after its packets: the shell now tells the
  drawing which stretch of packets is the frame's (`Drawing::frame_taken`: the open one, or the one closed at the last
  vblank).

## What is left of slice 4

- 4a proper: the kept frame scene drawn again at t into images of its own (stand-ins for every target it writes, the
  target copies and the first-draw image too), the native draws by their picture records (`NativeDraws::
  begin_frame_kept`, `picture_instance`), the GIF corners numbered by their kicks (as `pcr_canvas.cpp`'s
  `add_primitive`), the picture's buffers (`pcr::Between`), the next scanout reading the stand-in frame with the kept
  layers; `motion_compare.py` taught a rate (60, 240) with paraLLEl-GS run at it too.
- 4b: mouse look from the scene, the late turns measured and the margin sized from them, the reprojection.

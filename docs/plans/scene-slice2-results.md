# The scene renderer, slice 2: results

2026-11-20. Slice 2 of `scene-renderer-design.md` ("The slices"): the frame's layers. Built in `port/gs/scene/`
(`scn_build.cpp`), on `scene/slice2` from slice 1 (`c94437c2`, #186). The measurements are in
`~/Documents/darker-reality-measurements/2026-11-19-scene-slice2/` (its README entry).

## What it does

- **Clears as load ops, dead ones left out.** A clear is not drawn where the stream has it: it is held back until
  something uses what it cleared. The draw that does opens a pass whose load ops are the clear, its render area the
  clear's rectangle (at the pond in 2x widescreen both frame buffers are one target, and the frame's clear covers only
  its rows), or, where that cannot be, a clear in the draw's pass. An op, a read-back or a picture that uses the
  cleared pixels gets the clear as a step of its own. A clear that a later clear writes over first is never drawn: the
  swap's first clear, `GsDraw_ClearDepth` after the reflection (the swap's clear of Z is still held back), the depth
  aliases' clears that the next frame clears again. Held back clears last across scenes.
- **The alpha's reset folded into the frame's clear.** At `GsDraw_SetScreenAlpha(0)`, where the census saw nothing
  read the frame's alpha since its clear (every frame checked in slice 1), and the clear, the reset and every draw into
  the frame between are within the frame's rectangle: the clear writes the reset's alpha, the draws between write no
  alpha (their colour write masks lose A), and the reset is not drawn. The fold is per target and row (a reflection's
  clear between the frame's clear and its reset is not the frame's). Where it does not hold, the reset is drawn as in
  slice 1.
- **The brightness folded into the picture.** `Game_ApplyBrightness` (the frame drawn over itself, ten strips, each
  pixel read from its own place) is not drawn into the target and its copy of the frame is not made. It is kept, and
  each picture of the frame draws it, reading the target, where slice 1 copied the target out: the same draw and the
  same arithmetic, moved to the picture by its viewport (pcr_prim.vert, then `scn_picture_layer.frag`: pcr_prim.frag's
  path for what such a layer is and nothing more). The target is copied only where it may not draw (a pixel's band
  around its edges). Anything else that comes to the frame's pixels (a draw, an op, a read-back, a save state's
  memory) draws it into the target first, at its place in the stream; a clear over it makes it dead.
  It qualifies when every texel it reads is inside its own rectangle, worked out from its sprites' corners, Vulkan's
  coverage of them and the shader's rounding of UV to 1/256 (`reads_inside`): it reads V from 0 to 448 over Y from 0
  to 447.5, so at 2x the lower half of the frame reads the row below its own.
- **The history image.** A texture read of a frame whose brightness is kept (the knock-back's frame before,
  `Game_BlendPreviousFrame`) reads the history: an image of the frame's rectangle with the brightness drawn, made once
  for that frame, rather than the brightness drawn into the target.
- **The fills after the HUD are not folded** (MapDrawState's fill, `Game_DrawFade`, the dim, the pause's shade). Their
  pixels are Vulkan's blend's (`pcr_blend.h`): kept byte for byte they must be blended by the GPU over the frame, and
  every picture of the frame (two a game frame at 30, each vblank) would draw them again where slice 1 draws them once.
  The design's fold (each a byte, ((A - B) * C >> 7) + D in the final pass) is the GS's arithmetic, not the GPU's blend:
  nearer paraLLEl-GS, but not byte for byte slice 1's. That is for the owner to choose (below).

Developers' switches (all on by default): `KF4_SCN_HOLD_CLEARS=0` (each clear a step where met), `KF4_SCN_FOLD=0`
(the brightness drawn into the target), `KF4_SCN_FOLD_ALPHA=0` (the reset drawn), `KF4_SCN_LEAN_LAYER=0` (the
picture's brightness drawn by pcr_prim.frag). `KF4_SCN_DUMP_DRAWS=1` with `KF4_SCN_DUMP_AT` lists each pass's GIF
draws and clears, and passes show their render area. `--stats` prints a line
for all of it (`[stats] scene renderer layers`).

## The criteria

| criterion | measured | passed |
|---|---|---|
| Pictures byte for byte slice 1's: the layer inventory's 50 scenarios (2x, native widescreen) | 49 of 50 byte for byte; "start spin" differs, and differs as much between two runs of slice 1 itself (1.41 million pixels: its mouse spin is not the same from run to run) | yes |
| ... the 760-view tour at 1x (`map_tour.py --renderer scene` against `tours/scene_final`) | 760 of 760 views byte for byte | yes |
| A game frame 2.5-3 ms less at 2x in a real window (`world_plain.py --variants unorm,scene --scales 2 --rounds 2`) | 0.93 ms less at 099, 1.71 at the pond, 1.35 at the palace (`unorm,scene_ref,scene`, two rounds; below) | no |
| ctest | 37 of 37 | yes |
| ASan + UBSan, a tour's first maps (099, 009, 028, 024) at 2x | only the known reports (`SoundDriver_Remote`, the decomp's calls through function pointers of another type, and Granite's `memcpy` of no input attachments from a null pointer, also in slice 1's run); none in `port/gs/` | yes |
| The Vulkan validation layer with synchronization validation: the pond, the palace, the menus, the map change, the save point, the title, the knock-back (`fx prev`) | no error or warning (after one fix: a depth alias's whole-image clear was put in a pass whose colour target is smaller); every picture byte for byte slice 1's | yes |

What the counters say at the pond scenario (New Game, then 009; 1,000 vblanks headless, `[stats]`): 7,537 clears met,
4,470 drawn as load ops, 1,386 in a pass, 20 alone, 1,659 dead; all 722 alpha resets folded; the brightness kept 722
times, never drawn into the target, 720 dead (the frame cleared again). In the knock-back scenarios (`fx prev`,
`fx aim`) the frame before is read from the history in 526 frames each.

## The GPU's time

`world_plain.py --variants unorm,scene_ref,scene --scales 2 --rounds 2 --exe-for scene_ref=<slice 1's build>` in a real
window with the owner's settings (Vega 7, 2x with native widescreen, 30; `cost_final/`), ms a game frame, medians of two
rounds, slice 1 measured in the same rounds:

| scene | variant | 0x000 | 0x280, 0x700 | DATE | 2D | copies | picture and present | all |
|---|---|---|---|---|---|---|---|---|
| 099 | unorm | 2.55 | 0.25 | 0.04 | 1.84 | 0.46 | 5.34 | 12.05 |
| 099 | slice 1 | 2.61 | 0.27 | 0.06 | 1.77 | 0.44 | 5.71 | 11.74 |
| 099 | slice 2 | 3.01 | 0.29 | 0.06 | 0.50 | 0.54 | 5.54 | **10.81** |
| pond | unorm | 2.52 | 3.43 | 0.34 | 1.67 | 0.67 | 5.25 | 16.11 |
| pond | slice 1 | 2.53 | 3.48 | 0.39 | 2.77 | 0.65 | 5.53 | 15.41 |
| pond | slice 2 | 3.58 | 3.96 | 0.38 | 0.14 | 0.12 | 5.46 | **13.70** |
| palace | unorm | 3.48 | 2.66 | 0.24 | 1.77 | 0.39 | 5.43 | 14.70 |
| palace | slice 1 | 3.65 | 2.64 | 0.27 | 1.64 | 0.44 | 5.78 | 14.44 |
| palace | slice 2 | 4.04 | 2.65 | 0.27 | 0.32 | 0.10 | 5.70 | **13.09** |

0.93, 1.71 and 1.35 ms less than slice 1: not the 2.5-3 the slice asked for. The 2D falls by 1.3-2.6 ms and the copies
by up to 0.5, as planned; but the world's time grows by 0.4 ms (0x000) at 099 and the palace and by 1.5 ms at the pond
(0x000 and 0x280), and that eats a third to a half of it.

Each part switched off in turn (`cost_split/`, one round, before the picture's lean shader):

| switched off | 099 | pond | palace |
|---|---|---|---|
| the alpha's fold (`KF4_SCN_FOLD_ALPHA=0`) | +0.24 | +0.30 | +0.17 |
| the brightness's fold (`KF4_SCN_FOLD=0`) | +0.56 | +0.70 | +0.25 |
| clears held back (`KF4_SCN_HOLD_CLEARS=0`, which also takes the alpha's fold) | +0.47 | +0.05 | +0.90 |

- **The alpha's fold** saves the reset's pass (0.65-1.1 ms) but costs the world about 0.4: with the fold off, the
  world's time is slice 1's again. It is not the colour write masks' read of each pixel: writing alpha 0 by the
  blend's alpha factors with a full write mask instead (`cost_blend/`) gave the same times to 0.05 ms. Not found yet.
- **The brightness's fold**: with pcr_prim.frag drawing it, each picture cost 0.43 ms a game frame more than its copy
  out (two pictures a game frame at 30). `scn_picture_layer.frag` made the picture cheaper than slice 1's copy (5.54
  against 5.71 at 099): the GPU's copy of the target is slower here than a draw of it.
- **Clears held back**: at the pond the frame's clears are partial render areas (both frame buffers one target), not
  fast clears; there they save nothing measurable. At 099 and the palace, where the world_plain scenes start from a
  quick save and each frame buffer is a target of its own, they are fast clears.

Why less than the estimate: the estimate (0.47 + 0.66 + 1.75 + 0.42, `fullscreen-layers-inventory.md`) is the PC
renderer's with native draw off. Slice 1 already drew the swap's second clear and `GsDraw_ClearDepth` as clears (the
0.66 of dead writes was mostly theirs), the fills after the HUD (0.54 of the 1.75) are not folded (above), and a
picture is made each vblank, so what moves into the picture is paid twice a game frame at 30.

## The pond's frame, before and after

`KF4_SCN_DUMP_AT=2090` (2x with native widescreen; the frame at row 896 of the 1920x1856 target):

| | slice 1 | slice 2 |
|---|---|---|
| steps | 15 (10 passes, 5 ops) | 10 (8 passes, 2 ops) |
| the frame's clear | a clear in a pass (1920x896 of 1920x1856), and `GsDraw_ClearDepth` after the reflection another | the world pass's load ops, colour and depth, its render area the frame's rows; the second Z clear dead |
| the reflection's clear | a pass of its own with no draws, its depth alias cleared by an op | the reflection pass's load ops |
| `GsDraw_ClearDepth` before the arm | a clear in a pass | the HUD's pass's depth load op (the HUD writes Z) |
| the alpha's reset | a full-screen pass (alpha only) | folded into the frame's clear |
| the brightness | the frame copied (1920x1024 into a kept image), then drawn over itself | drawn by each picture in place of its copy out |

## What is left, and what slice 3 starts from

- **The world's 0.4 ms (1.5 at the pond) more with the alpha's fold**: found and taken (2026-11-20,
  `scene-slice2-after.md`): not the world's but the shadows' first-draw image's clear, timed in the world's last draw;
  the image is no longer cleared each frame.
- **The fills after the HUD**: folded (`scene-slice2-after.md`), byte for byte slice 2's: the GS's arithmetic came out
  further from paraLLEl-GS, the frame under the fills being a level off it already.
- **The frame's partial clears**: where both frame buffers are one target (the pond, from a New Game) a frame's
  clear is a partial render area, not a fast clear. Each frame buffer an image of its own (the design's "3. The
  targets") would make them whole-image clears.
- **The second picture of a game frame** at 30 draws the brightness again (as slice 1 copied the target again):
  slice 5's repeated vblank takes it. And since a draw of the target is cheaper than the GPU's copy of it here, the
  picture with no layer might be drawn rather than copied too.

## Tools

- `tools/scene_scenarios.py --reference-exe EXE`: the reference is that build's scene renderer (an earlier slice's)
  instead of the PC renderer.
- `tools/world_plain.py --variants unorm,scene_ref,scene --exe-for scene_ref=EXE`: a variant played by another build
  (slice 1's), in the same rounds.

# The scene renderer, slice 1: results

2026-11-19. Slice 1 of `scene-renderer-design.md` ("The slices"): `--renderer scene`, game frames at 30 drawn from a
scene (`port/gs/scene/`). What it had to show (`HANDOFF_scene_slice1.md`, "What it must show"), each criterion with its
measurement and whether it passed. The measurements are in `~/Documents/darker-reality-measurements/2026-11-19-scene-slice1/`
(its README entry).

## The criteria

| criterion | measured | passed |
|---|---|---|
| The tour at 1x (760 views) against paraLLEl-GS: no more than UNORM's 7,008 pixels over 24 levels, no view newly over 0.4% | 7,008 over 24 levels against paraLLEl-GS, the view over 0.4% the same one as `unorm`'s (099, 0.68%); all 760 views byte for byte `KF4_PC_DEPTH=unorm`'s (`tours/scene_final`, `compare/tour_cmp.json`) | yes |
| `motion_compare.py`'s 13 cases at 1x, 30: none further from paraLLEl-GS than `unorm`'s run | all 13 the same as `unorm`'s, case by case (`motion/scene`) | yes |
| Every scenario of the layer inventory draws (2x, native widescreen) | all 50 drawn to their end; 47 byte for byte the PC renderer's (`unorm`). The others: "start spin" (its mouse spin is not the same from run to run: it differed as much in the run before, 1.42 million pixels), and "fire elementals" and "combat", where the creatures' shadows overlap: at 1x (`scenarios_1x_final`, `--gs`) on the pixels where scene and pc differ the scene renderer is within 0.9 levels of paraLLEl-GS on average and the PC renderer 4.1-10.0 (it darkens overlapping shadows twice) | yes |
| The world's GPU ms at 2x in a real window (Vega 7, mains, balanced), within 10% of `unorm`'s | 099 2.87 against 2.79 (+3%), the pond 6.03 against 5.93 (+2%), the palace 6.30 against 6.12 (+3%) (`cost2/`, two rounds each, both measured in the same run) | yes |
| DATE under 0.1 ms | 0.06 at 099, 0.39 at the pond, 0.27 at the palace | no (see below) |
| No surface copies but the passes' | the PC renderer's copies and no more: 0.45 / 0.66 / 0.44 ms against `unorm`'s 0.46 / 0.67 / 0.39; texture copies from targets 884 at 009 (1x, 700 vblanks) against 886. A game frame at 009 copies the frame for the brightness pass, the reflection's target for the water that reads it, and the HUD's DATE sprite's rectangle: the last two are not the passes' | in part: the reflection's copy (a target read while nothing draws into it, which the scene could read without one) and the HUD sprite's are left |
| The census: the alpha read between the frame's clear and its reset | in none of the frames checked, every frame checked: 0 of 287 at 009 (1x), 0 of 876 at the palace (2x widescreen), 0 in every scenario | yes |
| ctest | 37 of 37 | yes |
| ASan + UBSan, a tour's first maps (099, 009, 028, 024) | only the reports `SANITIZERS.md` lists (`SoundDriver_Remote`, the decomp's calls through function pointers of another type); none in `port/gs/` | yes |
| The Vulkan validation layer on a few scenes | not run: `vulkan-validation-layers` is not installed here | not run |

The game frame's whole GPU time at 2x is less than `unorm`'s at all three: 11.66 against 12.02 ms at 099, 15.57 against
16.08 at the pond, 14.41 against 14.67 at the palace.

## DATE

The shadows by the first-draw image cost 0.39 ms a game frame at the pond and 0.27 at the palace (2x): their numbers
into the first-draw image (0.09) and the darkening by it (0.30 at the pond), each shadow drawn twice. That is over the
design's 0.1 ms. But `world_plain.py`'s DATE column for `unorm` counts only its shadows' draws (0.33, 0.24): its copies of
the frame for them are the `DATE copy` kind, 2.18 ms at the pond and 0.71 at the palace. Against DATE's whole cost the
scene renderer's is 0.41 against 2.51 ms at the pond and 0.28 against 0.95 at the palace.

Before slice 1's last fixes, at 2x with native widescreen half the frames' shadows went by a copy: the two frame buffers
are one target there (the back buffer 448 rows down it), and the frame's clear and `GsDraw_SetScreenAlpha(0)` were known
only at the target's top. They are now found by the frame's own rectangle and place (`scn_build.cpp`, `is_frame_clear`,
`is_alpha_reset`; the alpha's state kept by image and row). Every native shadow now goes by the first-draw image (the
palace: 69,657, none by a copy; the 507 left by a copy are the HUD's DATE sprite), and the census checks every frame.

## What made the GPU time

At 009 (1x, headless, 700 vblanks) the scene renderer first drew native 000 in 770 ms of GPU time against the PC
renderer's 315. Two causes, both fixed, the pictures byte for byte as before:

1. **A copy of a target for each native batch that read it.** The last batch's texture was kept for the next batch of
   the same state, but not a target's copy; each batch made its own, and each copy is an op that ends the pass. Now a
   target's copy is kept while nothing is drawn over its pages and no op writes, and the copies are made into one kept
   image a size and format, cleared only where an earlier copy reached past this one. 14,908 passes became 3,114 (the
   PC renderer: 2,546), 12,678 copies 884 (886), 2,077 ops 1,570.
2. **One draw a native batch, its state set each time.** A native batch with the last draw's material (in the same pass,
   the same buffers, viewport and scissor) takes that material, and the drawer sets nothing for it; where its triangles
   go on from the last draw's in the same record, the last draw is made longer, as the PC renderer's runs. DATE's
   shadows too: their numbers go on as their triangles do. 663,000 draws became 268,000 (the PC renderer: 217,000 calls).
   Native 000's GPU time: 383 ms against the PC renderer's 271-306.

## What slice 2 starts from

- **Clears in a pass at the pond (2x, from a New Game)**: both frame buffers are one target, so the frame's clear covers
  only part of its image and is a `vkCmdClearAttachments` in the pass, not a load op: 0.92 ms a game frame for its three
  clears, where the PC renderer's sprites take 0.54. Slice 2's clears as load ops (a render area the frame's rows) take
  this.
- **The 2D's time** (`world_plain.py`'s 2D column): a full-screen additive sprite's interval shows 0.68-1.10 ms against
  `unorm`'s 0.24, with the same count; the barrier after it is timed with it. The game frame's whole time is less than
  `unorm`'s.
- **DATE**: the darkening redraws every shadow with `pcr_prim.frag` to darken where its triangle came first (0.30 ms at
  the pond); one draw a run of shadows of one material, reading the numbers' range, would take it under 0.1 ms.

## The unidentified 0x000 GIF 3D

Found: the map's triangles that cross the GS's range, cut up on the EE (`ModelScreen_CutClippedTriangles`, the design's
"What still comes as GIF"). The scene renderer draws them as the scene's 3D items with `pcr_prim`'s arithmetic.

## Tools

- `tools/scene_scenarios.py --gs` (with `--scale 1`): paraLLEl-GS played too, the scene and PC renderers measured against
  it.
- `tools/motion_compare.py --renderer scene`: the scene renderer in the PC renderer's place.
- `tools/world_plain.py --variants scene`: a variant may carry the game's arguments (`layer_inventory.live_scenario`'s
  `args_more`).
- `KF4_SCN_DUMP_AT=VBLANK`: the scene closed at or after VBLANK and the two after it, step by step (passes, their images,
  load ops and draws; ops).
- `KF4_GPU_DETAIL=1`: the scene renderer's draws timed by shader state as the PC renderer's, its DATE passes and its
  clears in a pass named.

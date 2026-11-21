# The final pass: results (branch `scene/final-pass`, 2026-11-20)

The final pass is what turns the frame's picture into the window's: the scanout (the shown buffer, with slice 2's
kept layers drawn over it), the anti-aliasing (FXAA in the owner's settings), the scaling to the window (`best`: two 1D
Catmull-Rom passes, here only the one down), the sharpening (0.5), and the presenter's copy into the swapchain. It was
known as "Scanout", 2.75 ms a picture whatever the scene holds (scene-slice4-results.md, 3f's cheap tests).

All figures: the owner's settings, 2x (a 1920 x 896 picture, native widescreen), fullscreen on a 1920 x 1200 screen
(the picture 1920 x 1080 in it, bars above and below), Vega 7, `world_plain.py` in a real window, medians of two
rounds. Measurements in `~/Documents/darker-reality-measurements/2026-11-20-scene-final-pass/`.

## Each stage on its own (`parts/`, `presenter/`)

The `picture` and `present` GPU kinds split into their parts (`pcr_gpu_time.h`: `picture layers`, `antialias`,
`scale across`, `scale down`, `sharpen`, `slot copy`, `final in one`), and the presenter's own command buffer timed
(`present_vk.cpp`: `presenter copy`, `presenter overlay`, kinds of the profiler's GPU table while a profile run records;
`KF4_PRESENT_TIME=1` prints them). `world_plain.py` prints the parts as a second table, the presenter in a column of its
own. Both runs, 30 and 60, show two pictures a game frame (240 for 120 game frames), so ms a picture is half the tables'
ms a game frame. At the palace:

| Stage | ms a picture | Bytes moved | GB/s |
|---|---|---|---|
| The layers drawn over the picture (`picture layers`) | 0.39 | ~14 MB | ~35 |
| FXAA: the picture copied, then drawn from the copy | 1.09 | 27.5 MB | 25 |
| The scaling down (across is the picture's own width: left out) | 0.69 | 15.2 MB | 22 |
| The sharpening | 0.65 | 16.6 MB | 25 |
| **The presenter** (never in `GpuTimes` before) | 1.03 | clear 9.2 + copy 16.6 + the flash recorder | ~30 |
| All | **3.9** | ~100 MB | |

- **The final pass was 3.9 ms a picture, not 2.75**: the presenter's 1 ms had never been counted anywhere.
- Every stage moves memory at the same 22-30 GB/s as a plain copy does (the slot copy, with `sharp` scaling: 13.8 MB
  in 0.53 ms, 26 GB/s): each stage costs what its bytes cost.
- The presenter (`presenter/`): the flash recorder's blit (on by default) **0.30 ms a picture**; the swapchain image
  cleared whole for the bars of a 16:9 picture in a 16:10 screen 0.2; the copy itself about 0.5.
- Measurements only, not answers (FXAA off, sharpen 0, `sharp` scaling: `scene_noaa`, `scene_nosharpen`,
  `scene_sharp`): each takes its own stage's time away and nothing else.

## The bound, and the final pass in one: slower (`fused/`)

The bound said: if FXAA, the scaling and the sharpening were one pass reading the picture once and writing the
window's picture once, at the cost of the down pass alone (0.69), the final pass would be about 2.0 ms a picture, with
the presenter's copy gone too about 1.2 (of 4.2 at 240). **That bound assumed the one pass would cost what the cheapest
of the passes did; it did not, and it was not tested before building.**

Built: `PresentScaler::fused` and `shaders/pcr_present_fused.comp`, a compute pass of 16 x 16 tiles, each in shared
memory in steps: the source's pixels FXAA'd (FXAA's code moved into `pcr_fxaa.h`, shared with `pcr_fxaa.frag`), the
cubic across and down, the sharpening. **The same pictures byte for byte** (`tools/final_pass_compare.py`: six motion
cases at 1x, 2x and 4x, both renderers), which took two findings:

- **RADV writes an 8-bit UNORM or a 16-bit float colour attachment through half floats rounded toward zero**
  (`v_cvt_pkrtz_f16_f32`, then the colour block rounds). A float rounded straight to 8 bits differs in about 4% of
  pixels by a level; the compute pass rounds as the attachment does (a compare with a table of each value's least
  half float).
- **The sampler's sRGB decode is not the correctly rounded one**: a table worked out on the CPU in doubles differed
  in a few pixels of a hundred. The compute pass decodes through a 256 x 1 image read with the sampler.

But in a real window it took **5.9-6.2 ms a picture against the passes' 2.3** (`fused/`), and headless (100 pictures,
`bench.sh`) every attempt to make it lean left it slower:

| The final pass in one, headless, ms a picture | All | No FXAA, no sharpening |
|---|---|---|
| first form | 4.83 | 2.55 |
| the tables in shared memory | 4.82 | 2.66 |
| weights a row, no integer division, the rounding a compare | 5.16 | 2.77 |
| shared memory halved (4 waves a SIMD, was 3) | 3.49 | 1.91 |
| ... FXAA's code left out (44 registers) | (3.08 without FXAA) | 1.88 |
| the passes it stands in for | 2.29 | (down alone 0.74) |
| a dispatch that only writes the image | | 0.32 |

The write alone is 0.32, so it is not the image; the tables, the division and the weights were not it either. What
moved it was occupancy (3 to 4 waves a SIMD: 5.2 to 3.5 ms): each tile's steps, between barriers, wait on memory
with too few waves on the SIMD to hide it, where the fragment passes run many. On this GPU the passes win. The final
pass in one stays as a developers' switch (`KF4_PC_FUSED=1`, `KF4_PC_FUSED_DROP=N` to leave its parts out), off.
Where a GPU has the occupancy and the cache to spare (a 9070 XT's 64 CUs and 64 MB of Infinity Cache) it would likely
do better, but there the whole final pass is a fraction of a millisecond and not worth one.

## What was kept (`after/`)

- **FXAA without its copy**: FXAA reads the picture and draws into a spare picture of the same kind, which is the
  picture from then on (`AntiAliasFrame::into`, `PictureOut::spare_picture_`); the temporal methods are as before.
- **The presenter clears the bars alone**, as rectangles in the render pass it draws the overlay in, not the whole
  swapchain image (`present_vk.cpp`; the whole image only when there is no picture).

Both byte for byte (`same_pp/`, `same_pp_pc/`: 1,125 pictures by the scene renderer, 432 by the PC renderer, against
the build before them). In a real window, ms a picture at 30:

| | before | after | saved |
|---|---|---|---|
| 099 | 4.02 | 3.57 | 0.44 |
| the pond | 3.78 | 3.46 | 0.33 |
| the palace | 3.91 | 3.50 | 0.41 |

FXAA 1.09 to 0.86 (headless it halved: 0.95 to 0.54), the presenter 1.05 to 0.88. **About 10%: not the structural
win this was meant to be.** At 240 the final pass is still about 3.5 of the 4.2 ms a picture may cost.

The validation layer with synchronization validation (`validation/`): combat at 60 headless, the passes and the final
pass in one, and 2,000 pictures in a real window with the owner's settings: no message.

## What is left, and what each could give

- **The presenter's copy (0.5 ms)**: the sharpening drawn by the presenter straight into the swapchain image in place
  of its copy (a raw Vulkan pipeline in `present_vk.cpp`), the renderer's sharpening pass gone; `--dump-present` and the
  flash recorder would have to show the same. About 0.5 ms a picture.
- **The flash recorder (0.3 ms a picture, on by default)**: the owner's to decide, a player's feature.
- **The scaling down (0.69) and the sharpening (0.65)** stay two passes: the sharpening needs its neighbours scaled,
  and a fragment pass making five scaled pixels for each would cost more arithmetic than this GPU has to spare.
- With all of it the final pass would be about 2.5 ms a picture at 2x on this screen. A larger cut is in how many
  pixels there are (the picture at 2x and the window's 1920 x 1080 are each 2 million), not in how they are passed.

Next, as before: 4b (mouse look, the third redraw), then the GS's pixel in the shader.

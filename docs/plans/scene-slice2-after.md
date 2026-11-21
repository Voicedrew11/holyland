# The scene renderer after slice 2: the alpha fold's cost found, the fills folded

2026-11-20, on `scene/slice2` after `69922070`. What `scene-slice2-results.md` left first ("What is left"): why the
world's time grew 0.4 ms (1.5 at the pond) with the alpha's fold, and the fills after the HUD folded into the picture
(the owner's decision 5). The measurements are in `~/Documents/darker-reality-measurements/2026-11-20-scene-alpha-fills/`
(its README entry).

## The world's 0.4 ms: not the world's

Each draw's GPU time at 099 (`KF4_GPU_DETAIL=2 KF4_GPU_DRAWS=FILE`, medians over 99 pictures, `draws/`), the alpha's
fold off and on:

| interval | fold off | fold on |
|---|---|---|
| the world's draws, but its last | the same to a few µs | the same |
| its last draw (native 000) | 14 µs | **528 µs** |
| the alpha's reset (2D, full screen, alpha only) | **587 µs** | (folded) |
| the shadows' passes after it | the same | the same |

No world draw got slower. A timer's interval runs from one timestamp to the next, and the start of a pass (its load
ops) falls in the interval of whatever was timed before it. The shadows' first pass clears the first-draw image (R32F,
1920x896) as its load op: with the reset drawn, that clear was in the reset's 2D interval; with the reset folded, in the
world's last draw. The clear was 0.5 ms of it, the reset itself about 0.07. Skipping the clear (a trial, pictures wrong)
took the interval from 528 µs to 22 µs.

Ruled out on the way (`exp1/`, `first1/`): the colour write masks the fold puts on the world's draws (no masks: the
same times), and the clear's value (1.0, which the GPU may clear fast, against 3e38: the same).

The pond's 0x280 and 0x000 growth in slice 2 is the same mechanism: the frame's clears became the world pass's load
ops, timed in the draw before them (there the reflection's 0x280 draws).

### What changed

- **The first-draw image is not cleared each frame.** A frame's triangle numbers are a generation's:
  `(first_generations - 1 - generation) << 17` plus the triangle's, written as n / 2^24 (exact in float below 2^24).
  A later frame's numbers are below every earlier frame's left in the image, so they win the MIN blend, and none is
  this frame's; the image is cleared to 1.0 when the generations run out, once in 128 frames (and when it is made).
  At most 131,072 shadow triangles a frame (else DATE by a copy, as past 2^24 before). `[stats] scene renderer DATE`
  counts the clears; `KF4_SCN_FIRST_KEEP=0` clears it every frame (`world_plain.py`'s `scene_firstclear`).
- **A pass's load-op clears timed as clears**, a developers' switch (`KF4_SCN_TIME_CLEARS=1`: `surface`, which
  `world_plain.py` counts in "copies"). Off by default: each is a timestamp more, and a timestamp waits for the GPU; on,
  the palace's frame measured 0.5 ms longer (`KF4_GPU_DETAIL=0`, 13.26 against 12.78). Without it a pass's load-op
  clears stay in the interval of the draw before them, as in slice 2's tables.

## The fills after the HUD folded

`is_fill` (`scn_build.cpp`): untextured sprites of one colour tiling the whole frame, no fog, no Z, no DATE, one pass
writing every pixel's colour, a blend of Vulkan's form: MapDrawState's fill (black at alpha 2 every frame at 099 and the
first maps), `Game_DrawFade`, the message dim, the pause's shade. A fill is kept as a layer; fills kept one after another
stay kept, and so do they when the brightness follows. The picture then draws the brightness reading the frame with the
fills applied to each texel it reads (`scn_picture_layer.frag`, the fills in its uniform block where a CLUT would be),
and the fills' own draws over the bands the brightness may not draw (its last row is not drawn: it shows the frame with
the fills; without them that row lacked the fill, 1,920 pixels over 24 levels in `fx fill`). Anything else that comes to the frame's pixels draws them into the target first, as any layer. Developers'
switch: `KF4_SCN_FOLD_FILLS=0`. `[stats] scene renderer layers` counts them.

### Which arithmetic

The owner's decision 5 was the GS's arithmetic (((A - B) * C >> 7) + D, floored), provided the changed pixels come out
no further from paraLLEl-GS. They come out further. `scene_scenarios.py --scale 1 --gs` with slice 2 as the reference
(`scen_gs/`), against paraLLEl-GS, pixels differing and the mean error where slice 2 and the fold differ:

| scenario @ vblank | slice 2 | the fold, GS arithmetic | slice 2 mean | fold mean |
|---|---|---|---|---|
| start @ 1803 (099, the black fill) | 224,121 | 253,014 | 1.18 | 1.32 |
| walk @ 1918 | 241,148 | 251,559 | 0.93 | 1.04 |
| pickup @ 1835 | 198,704 | 249,016 | 0.97 | 1.06 |
| fx fill @ 1803 (red 0x40) | 193,407 | 194,563 | 1.09 | 1.09 |
| fx fade @ 1803 | 222,222 | 252,781 | 1.19 | 1.32 |
| spell 76 @ 1928 | 276,955 | 236,746 | 1.69 | 1.21 |
| teleport @ 1815 | 154,362 | 67,275 | 0.83 | 0.36 |
| map change @ 1883 | 91,507 | 255,407 | 0.37 | 1.17 |

The arithmetic is paraLLEl-GS's (its `alpha_blend` is `floor((A - B) * C / 128) + D`, and these fills have no DTHE).
What it is applied to is not: our frame under the fill is already off paraLLEl-GS's by about a level. At 099 the fold
is about 1 level darker than slice 2 everywhere (the black fill at alpha 2 takes 1 from every channel of 1-64, where
the GPU's rounding takes none), and in most 40x32-pixel blocks of the world slice 2 sits about +0.5 above paraLLEl-GS and the
fold about -0.5 below it. At the palace, with no fill, slice 2 is a uniform -0.98 below paraLLEl-GS, 16% of channels
exact (`scen_gs2/`). So the GS's floor, right in itself, moves 099's pictures off paraLLEl-GS by one more level in more
places.

**So the fold makes slice 2's pixels.** The picture's shader does what slice 2's draw of the fill did, form by form
as `pcr_blend.h`'s `choose_blend` draws it (`scn_picture_layer.frag`'s `gpu_blend`): Vulkan's blend by the factor
C / 128, or C applied in the shader first ((Cs * C) >> 7) and added or taken away. The blend's own arithmetic is the
Vega's (RADV), fitted to slice 2's pictures: a trial build drew the frame as the picture read it before the fill
(`scen_probe/`), and against slice 2's picture of the same frame each value under the fill gives what the blend made of
it (through the brightness's (y * 136) >> 7, which here loses nothing). Plain float with rounding to the nearest missed
on ties; a factor held in 8 bits missed elsewhere. What fits all 1,251 pairs (black fills at alphas 0x21, 0x4e and
0x6e, red 128 at 0x40), with no miss: each colour as a fraction of 255 held to 12 significant bits (to the nearest, a
half to even: done on the float's bits, since `log2` and `exp2` a channel made the picture 0.15 ms dearer), the sum
exact, the result rounded to the nearest 255th (a half up). Another GPU's blend may round
otherwise, as slice 2's pictures would then differ by a level here and there too.

The GS's arithmetic stays a developers' switch (`KF4_SCN_FILL_GS=1`), to be measured again once the level under the
fills is found.

### Pictures

- **Byte for byte slice 2's**: the layer inventory's 50 scenarios at 2x with native widescreen (`scen_final/`,
  `--reference-exe` slice 2): 49 of 50; "start spin" differs, as it does between two runs of slice 2 (its mouse spin).
  The first-draw image's generations alone (`KF4_SCN_FOLD_FILLS=0`, `scen_nofills/`): 49 of 50, the same.
- After the last change to the shader (the 12 bits on the float's bits): the scenarios with fills (start, fx fill,
  pickup, teleport, flesh, spell 81) byte for byte again.
- **The Vulkan validation layer with synchronization validation** (`validation/`): start, the pond, the palace, fx
  fill, fx dim, fx prev (the history), pickup, the map change: no error or warning, every picture byte for byte.
- ctest: 37 of 37.

## The GPU's time

`world_plain.py` at 2x in a real window with the owner's settings (Vega 7, 30), slice 2 (`--exe-for scene_ref=`) and
this build in the same rounds, medians of two rounds, ms a game frame (`cost_final/`, `cost_fx2/`):

| scene | slice 2 | now | less | the first-draw image cleared each frame | the fills drawn into the target |
|---|---|---|---|---|---|
| 099 (from the quick save: the load's fade only) | 10.77 | 10.42 | **0.35** | 10.82 | 10.40 |
| the pond (a fill every frame) | 13.72 | 13.11 | **0.61** | 13.75 | 13.15 |
| the palace | 12.95 | 12.78 | **0.17** | 12.96 | 12.83 |
| fx fill (red 0x40 every frame) | 10.88 | 10.28 | **0.60** | | 10.47 |
| fx dim (the message dim every frame) | 10.96 | 10.38 | **0.58** | | 10.57 |

- **The first-draw image's generations**: 0.35-0.6 ms a game frame wherever the shadows are drawn (the clear is gone;
  the shadows' DATE draws cost 0.16-0.19 more, their MIN blend reading the image's old numbers where a cleared image
  read nothing).
- **The fills**: 0.19 ms a game frame where a fill covers the screen (2D 0.35 less, the picture 0.15 more: it is made
  each vblank, two a game frame at 30), 0.04 at the pond. Not the 0.5 asked: the second picture of a game frame pays
  the fills again, as it pays the brightness (slice 5's repeated vblank takes it).
- So slice 2 with these: 0.9 + 0.35 = 1.25 ms at 099, 1.7 + 0.6 = 2.3 at the pond, 1.35 + 0.17 = 1.5 at the palace less
  than slice 1.

## What is left

- **A level off paraLLEl-GS everywhere at 1x** (`scen_gs2/`: the palace -0.98 a channel, 16% exact; 099's world
  +0.5): small, but under every picture, and what stands between the fills' GS arithmetic and paraLLEl-GS. Not
  counted by the tours (pixels over 24). Worth finding before slice 3's fused passes are judged against paraLLEl-GS, since
  their pixel changes are of the same size.
- **The frame buffers as one target at the pond**: the owner chose an image each (decision 6), and it was built: the
  target split where a frame is asked for 448 rows or more down it (New Game's first frame draws 897 rows at FBP 0,
  making FBP 0 and 140 one target; one split a run). Pictures byte for byte, the validation layer clean, but no gain:
  the pond 13.14 ms a game frame split against 13.10 not, the palace and 099 the same (two rounds,
  `~/Documents/darker-reality-measurements/2026-11-20-scene-split/`). The partial render area's clear costs no more
  than a whole image's here. Not kept. (On the way: the first-draw image, made again whenever the target's size
  changed, was then made and cleared every frame, the two buffers being 896 and 960 rows: worth keeping in mind if the
  targets change.)
- **Slice 3** (`scene-renderer-design.md`, "5. Draws"): see "Slice 3's start".

## Slice 3's start

- **0x280's two passes as one draw apply to every sphere batch counted.** Every 0x280 batch with a sphere pass has its
  draw's colour scale.w 0 and bias.w 128 (the pond: 128,414 batches over a run; the palace: 72,872; a temporary counter
  in `pcr_native.cpp`'s `draw_280`): its first pass's alpha is 0x80 at every corner, and its blend, (Cs - Cd) * As + Cd,
  is the source alone. The sphere pass is then (Cd * As2 >> 7) + Cs2 over a colour the shader knows: one draw, no
  blend, the GS's arithmetic exact but for triangles of one batch on the same whole Z.
- **The PC renderer's `PCR_RECORDS` shaders** (`pcr_*_rec`: a draw's fields from a records buffer by `gl_DrawID`,
  textures from the bindless table `u_table`, CLUTs from one buffer `clut_at`, merged runs of native batches as one
  draw) are the batching's shaders: the scene's draws as records, each run of one pipeline one indirect draw.
- **The timers' cost**: `KF4_GPU_DETAIL=1` writes a timestamp (all commands) at each change of shader state, and the
  0x280 batches change state with every pass; their measured time includes those waits. Slice 3's numbers should be
  read from a run's GPU time with the detail off too.

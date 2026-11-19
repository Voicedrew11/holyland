# Depth by layers: the proof of concept

2026-11-19. The step `HANDOFF_depth_by_layers.md` set, the first of `renderer-scene-not-gs.md`'s order: before the
scene renderer is built, whether the GS-exact depth (the depth rule, exact Z's planes: half the world's GPU time) can
go, because the draws that can tie are known from the game's data and everything else can use the GPU's own depth
test. Developers' switches and counters only; nothing a player gets (the default path's pictures are `main`'s byte for
byte: below, "Checks").

**The owner's decision after it (2026-11-19): no change to today's renderer; the scene renderer is built next, and
takes its depth from here** (`renderer-scene-not-gs.md`, `HANDOFF_scene_renderer_design.md`). Where this says the UNORM
depth could go into today's renderer, read it as a measurement of what the scene renderer's depth will do.

## The answer, short

**The premise is half right, and the conclusion is better than it asked for.**

- **Ties are not knowable from the data.** The depth rule's ties are spread over the whole world: in a whole-game
  tour (760 views) about 40% of the map records drawn as layers and a third of the opaque ones tie with something in a
  picture, and 35% of the fragments that tie (not at the same depth exactly) are not decals at all: opaque and
  covering map records meeting other records, map objects, and, far from the eye, unrelated surfaces that land on the
  same whole Z of the GS's 16-bit Z buffer (013's far tree and the rock face before it). A scene renderer cannot list
  them when the map loads.
- **But nothing needs to know them.** The GS's ties come from its Z being a whole number of a 16-bit Z buffer. A
  **16-bit UNORM depth buffer** (`D16_UNORM`, every GPU has it) rounds each pixel's depth to the same whole steps in the
  GPU's fixed-function depth unit, so the GS's ties are ties there too and the GPU's own GEQUAL lets the later draw win,
  **with the early depth test kept, no shader writing depth, no interlock, no copy, no planes, no bias**. With every
  draw's Z clamped at each pixel (Vulkan's depth clamp to the viewport's range, as the GS clamps), it draws the
  whole-game tour **closer to paraLLEl-GS than the depth rule does** (7,008 pixels over 24 levels in 760 views,
  against the rule's 8,845; where it differs from the rule, 1,813 pixels move nearer paraLLEl-GS and 25 further), and
  every case of play in the motion comparison nearer too.
- **The bias for known layers (the handoff's first form) is worse than no bias**: 1 Z for the decals fixes about 150
  pixels of the tour and breaks about 500 (a puddle drawn through the floor far from the eye, where 1 Z is a long way);
  a slope bias breaks 16,000. Plain GPU depth with invariant positions and the per-pixel clamp is already within 1,816
  pixels of the rule over the tour; the UNORM depth closes the rest.
- **Cost**: the world with the UNORM depth costs what the GPU's depth costs: at the owner's 2x on the Vega 7, 2.79 /
  5.95 / 6.08 ms a game frame at 099, the pond and the palace against today's 5.37 / 10.30 / 9.31 (below, "What it
  costs"; the experiment's DATE by a copy costs 0.3-1.9 ms outside the world, which the scene renderer must avoid).
- **Overdraw**: the world shades about 3.1 (099) to 6.2 (the pond) fragments a pixel at 2x, and the game does not draw
  back to front or front to back; much of it is layers that must be shaded anyway (below).

**Recommendation** (the owner decides): make the scene renderer's depth the GPU's own test on a depth buffer of the GS
Z buffer's own precision: `D16_UNORM` for the game's 16-bit Z buffers (every 3D draw counted uses PSMZ16S), the depth
`Z / 65535`, clamped at each pixel by the depth clamp; and a 32-bit float depth for the 24- and 32-bit Z buffers (the
menus'), where the GS's whole steps are finer than anything the game draws close. "Exact" for depth would then mean:
**the GS's whole Z at each pixel, rounded by the GPU** — the same whole numbers, the same ties, the same winner,
except where the GPU's interpolation and the GS's land on either side of a half (single pixels along steep edges).
What it needs from the scene renderer: DATE without the depth buffer's stencil (D16 has none; the entity pool's
shadows), and the 32-bit-Z draws on a float depth.

## How it was found

### The counters (developers' switches)

- **The tie counter**, `KF4_PC_TIES=FILE` (with the depth rule, the default): every draw the rule tests counts, in its
  own slot (each run of a native batch: one RdDraw's, whose kind and identity the batch now carries), what the rule
  decided for each fragment (`pcr_prim.h`'s `pcr_count_ties`): tested, passed, and the **ties** (within the rule's
  tolerance): passed at the same depth exactly ("equal": the same plane drawn again), passed from behind, passed from
  ahead, lost; and the near ones, lost or passed within 2 Z past the tolerance; and its Z's range. Read back a few
  vblanks later (a ring of four, a fence each) and written at the vblanks `KF4_PC_TIES_AT` lists, with the draw's
  program, kind, owner, part, GS state; `KF4_PC_TIES_ORDER=1` writes every slot (the draw order), `W` lines sum each
  program's.
- **The overdraw counter**, `KF4_PC_OVERDRAW=FILE`: each native batch's fragment shader invocations, a pipeline
  statistics query around its draws (`pipelineStatisticsQuery` now turned on where the GPU has it,
  `cmake/granite_depth_bounds.cmake`), by program a vblank.
- Both are `pcr_canvas.cpp`'s `Counters`, on the game's canvas only (not the pump's), and leave a kept walk's runs
  undrawn while counting. `map_tour.py --ties` and `motion_compare.py --ties` set them for each view or compared
  vblank; `tools/depth_ties.py` reads them (a tour, a motion comparison, or `--scenes` for the overdraw by mode).
- **The depth modes**, `KF4_PC_DEPTH=` beside `rule|exact|gpu`:
  - `layers`: the GPU's depth test for every draw (no exact Z, no rule, no planes), every draw's Z clamped at each
    pixel (`PCR_F_PIXEL_CLAMP`: the depth Z / zmax, the viewport's range 0 to zmax / 2^24, Vulkan's depth clamp;
    `KF4_PC_PIXEL_CLAMP=0` at the corners as `gpu`), and the layers (`KF4_PC_LAYERS=decal`, the default: `is_decal`;
    or `blended`) drawn `KF4_PC_LAYER_BIAS="Z[,SLOPE]"` nearer (1 Z by default, added to each corner in the vertex
    shader, `PCR_F_LAYER`; SLOPE as Vulkan's slope bias in GS pixels).
  - `unorm`: as `layers` with no bias, the depth surfaces of the 16-bit Z formats `D16_UNORM` and the others' D32F,
    no stencil (DATE by a copy).
- Every vertex shader's position is now `invariant` (`pcr_common.h`): a draw and a layer over it from the same corners
  get the same depth in any pipeline, which the GPU's depth needs. The default path's pictures are unchanged by it.

The runs: the whole-game tour (`map_tour.py --positions 3`: 64 maps, 760 views) at 1x with native draw on at 30
(`--native-scale 1 --no-native-widescreen --native-draw on --frame-rate 30`), each mode and paraLLEl-GS
(`--renderer gs`); `motion_compare.py`'s 13 cases at 1x, the same options; the overdraw at the owner's 2x with native
widescreen (1920 x 896) at three scenes. Raw output: the owner's
`~/Documents/darker-reality-measurements/2026-11-19-depth-by-layers/` (its README).

## 1. Which draws tie, in play, at every map

The depth rule's ties at the tour's 760 pictures (1x):

| | fragments | of those passed |
|---|---|---|
| tested by the rule | 595,167,450 | |
| passed | 391,419,934 | |
| ties at the same depth exactly ("equal") | 25,799,818 | 6.59% |
| ties passed from behind | 105,404 | 0.027% |
| ties passed from ahead | 188,364 | 0.048% |
| ties lost (GREATER, the corners' bound) | 2,245 | |
| lost within 2 Z past the tie | 601,865 | 0.15% |
| passed within 2 Z past the tie | 1,271,081 | 0.32% |

- **Equal ties are the same plane drawn again** (a layer over its own surface from the same corners, a second pass):
  6.6% of everything drawn, 98% of them decals of the map's records (099's rock shading: 2.3 million at 099's twelve
  views). The GPU's own GEQUAL ties these exactly, given invariant positions.
- **The ties that are not equal** are what the GPU's depth may decide otherwise: 0.078% of the passed fragments over
  the tour, on every map: most at 032 (0.78% of its fragments: a covering map record over another), 033 (0.50%), 014
  (0.46%), 114 (0.24%: opaque records), 031, 000; least at 103, 023 (0.01%).
- In the motion cases (307 pictures: creatures, spells, menus, the title, under water): equal ties 26% (the first map's
  rock shading fills most of their pictures), not equal 0.069%, the creatures' layered parts (program 0x280's entity
  decals) among them in combat and before the fire elementals and salamanders.

By class of draw (the tour; slots with ties):

| class | non-equal tie fragments | equal tie fragments |
|---|---|---|
| map record, decal (blended, GEQUAL), writes Z | 126,633 | 24,033,612 |
| map record, covering blend (As 0x80), writes Z | 53,953 | 72,741 |
| map record, opaque, writes Z | 32,860 | 295,600 |
| map object (program 0x280), decal | 46,452 | 7,222 |
| map object (program 0x000), decal / opaque / covering | 7,810 / 11,725 / ... | |
| map record, decal, no Z write | 7,085 | 1,065,446 |
| creatures (0x280 entities), effects (0x700), items, the ripple, GIF triangles | the rest, a few thousand each | |

(Every class tests Z GEQUAL; "alpha tested" throughout: the game's TEST is ATST GREATER AREF 0 everywhere, a cut-out.)
The full tables: `depth_ties.md` in each tour's folder.

## 2. Are they identifiable before drawing?

**Not as layers over a known surface.** Decals (`is_decal`) hold 65% of the non-equal tie fragments; the other 35% are
covering and opaque map records, map objects and creatures. And within a class, tying is the rule rather than the
exception: counting every draw at ten maps (`KF4_PC_TIES_ORDER`, 120 views), **40% of the map records drawn as decals
(377 of 944 draws a picture), 34% of the opaque ones (191 of 559) and 27% of the covering ones** tie with something,
most by a few hundred fragments along a seam where two records overlap. What ties with what is decided by where the
camera is as much as by the data:

- **Far from the eye the GS's Z is coarse.** The game's 3D uses a 16-bit Z buffer (PSMZ16S, 0x3A, for every
  depth-tested draw counted), and Z falls as 1 / distance, so far away one whole Z covers a long way, and unrelated
  surfaces share it: the GS calls it a tie and the later draw wins. At 013 (view 10) a far tree trunk and the rock face
  in front of it are on one whole Z (about 400); the GS draws the rock over it (later), the GPU's depth, keeping its
  fraction, the tree through the rock (`compare/crops/013_full.png`). No list of layers made when the map loads can
  know which surfaces will share a whole Z from where the player stands.
- The native GS's PC world (P2, P7 in `PC_RENDERER.md`) drew "coplanar layers by order and a depth bias" with an
  invariant vertex stage; its tolerant layer Z, tried on the PC renderer (`PC_RENDERER.md`, "Layer Z"), made things
  worse where it changed anything (map 004's leaves, 026's wall corner). The same shows here: below.

## 3. What the GPU's depth costs and changes

### Pictures: the whole-game tour against paraLLEl-GS

Each mode's tour against paraLLEl-GS's (`tour_compare.py`), and against today's rule pixel by pixel: the pixels that
change by more than 24 levels, and whether each moves nearer paraLLEl-GS or further.

| mode | pixels over 24 against paraLLEl-GS (760 views) | views over 0.4% (blurred) | changed from the rule, over 24 | nearer / further |
|---|---|---|---|---|
| rule (today) | 8,845 | 1 | | |
| `gpu`: the GPU's depth, Z clamped at the corners | 16,740 | 3 | 9,922 | 658 / 9,264 |
| `layers`, bias 0: the GPU's depth, Z clamped at each pixel | 9,353 | 1 | 1,816 | 657 / 1,159 |
| `layers`: ... and the decals 1 Z nearer | 9,747 | 1 | 2,488 | 814 / 1,674 |
| `layers`, slope bias 1 GS pixel, no constant | 24,265 | | 16,537 | 718 / 15,819 |
| `layers` 1 Z, corners clamped (`blended`: every blended GEQUAL draw) | 17,156 (16,945) | | 10,614 (10,290) | 815 / 9,799 |
| **`unorm`: D16_UNORM depth, Z clamped at each pixel** | **7,008** | **1** | 1,838 | **1,813 / 25** |

(The one view over 0.4% in every mode is 099 v6, 0.62%: not depth.) The kinds of difference, each with a crop in
`compare/crops/` (rule | modes | paraLLEl-GS, x3):

- **Z clamped at the corners** (`gpu`): map 112's rock near the eye has corners past the 16-bit Z buffer's largest;
  clamped at the corners its Z falls across the triangle and it loses to what is behind (6,215 and 1,883 pixels at
  views 10 and 11, `k_112_1220.png`). The GS clamps each pixel; Vulkan's depth clamp to a viewport range of the Z
  buffer's largest does the same (the `layers` modes, `unorm`): gone. Not a tie at all.
- **Unrelated surfaces on one whole Z far away** (`gpu`, `layers`): 013's tree through the rock (325 pixels at v10, 164
  at v9). Fixed only by the UNORM depth, which rounds as the GS does.
- **A layer pushed through** (the bias): 028's puddle, 1 Z nearer, shows through the floor in front of it far from the
  eye, where 1 Z is a long way (162 pixels, `k_028_720.png`); the slope bias does it everywhere a layer is steep (036,
  024, 042, 101, 037: hundreds of pixels each).
- **Crossing surfaces** (`gpu`, `layers`): where two different surfaces cross, the GS's whole Z picks a winner along
  the line that the fraction picks otherwise: 024's lava edge, specks of rock over the lava (54 pixels at v13,
  `k_024_1320.png`), 101, 037, 040, 015: tens of pixels a view. The UNORM depth rounds like the GS and draws these as
  paraLLEl-GS does.
- **What UNORM still changes for the worse**: 25 pixels in the whole tour (100 v9: 14; 016, 111, 115, 031: a few):
  single pixels where the GPU's interpolation and the GS's land on either side of a half Z.

### Pictures in motion

`motion_compare.py`'s 13 cases at 1x (745 pictures), each mode's against the rule's, pixels over 24 levels moving
nearer paraLLEl-GS or further:

| case | `layers`, bias 0: nearer / further | `unorm`: nearer / further |
|---|---|---|
| combat | 128 / 262 | 464 / 6 |
| first kill | 14 / 10 | 26 / 0 |
| fire elementals | 246 / 344 | 704 / 408 |
| salamanders | 524 / 2,940 | 966 / 4 |
| watchers | 144 / 0 | 480 / 0 |
| spells 3, 16 | 240 / 492 | 2,122 / 0 |
| under water, drain | 266 / 118 | 1,072 / 0 |
| title | 24 / 48 | 210 / 0 |
| menus | 155 / 69 | 505 / 76 |
| all 13 | 1,741 / 4,283 | 6,549 / 494 |

- The UNORM depth draws every case nearer paraLLEl-GS than the rule but one: at the fire elementals 408 pixels are
  further, a creature's shadow (the entity pool's DATE draws) darker on the floor (`compare/crops/fire_elementals_unorm.png`).
  That is DATE drawn by a copy, which the experiment needs (D16 has no stencil): the rule drawn so
  (`KF4_PC_STENCIL=0`) differs from today's by 402 pixels in that case, the same shadow. The scene renderer's DATE is
  to be settled anyway (below).
- With every Z buffer made 16-bit (the experiment's first form) the menus lost the world behind them (4.3 million
  pixels): their 32-bit Z buffer squeezed into 16 bits. Each Z buffer's depth is now of its own format's kind
  (`depth_format_for`: D16_UNORM for PSMZ16 and PSMZ16S, D32F for the others); the tour is the same either way.
- Plain GPU depth with the per-pixel clamp (`layers`, bias 0) moves more pixels further than nearer (salamanders: 2,940
  further): the far surfaces sharing a whole Z, in motion.

### What it costs

`world_plain.py --variants exact,gpu,layers,unorm --rounds 2` (a real window, the owner's settings, the Vega 7 on mains
power and the balanced profile; the medians of two rounds, each within 0.12 ms at 1x and 2x and 0.78 at 4x). The world
(programs 0x000, 0x280, 0x700 and exact Z's planes), GPU ms a game frame, and the whole frame in brackets:

| Scene | Scale | exact (today's rule) | gpu | layers (1 Z, clamped at each pixel) | unorm |
|---|---|---|---|---|---|
| 099's start | 1x | 3.09 (9.04) | 1.64 (7.34) | 1.65 (7.33) | 1.63 (7.49) |
| 099's start | 2x | 5.37 (15.66) | 2.82 (11.68) | 2.84 (11.78) | **2.79** (12.02) |
| 099's start | 4x | 17.19 (66.76) | 10.91 (54.26) | 10.84 (54.28) | 10.87 (57.63) |
| the pond | 1x | 6.51 (12.27) | 4.27 (9.64) | 4.31 (9.64) | 4.26 (10.18) |
| the pond | 2x | 10.30 (20.31) | 5.90 (14.25) | 5.88 (14.24) | **5.95** (16.11) |
| the pond | 4x | 30.99 (74.86) | 18.98 (57.62) | 19.13 (58.50) | 18.99 (64.72) |
| the palace | 1x | 7.06 (12.83) | 4.96 (10.22) | 4.96 (10.23) | 4.95 (10.39) |
| the palace | 2x | 9.31 (19.31) | 6.06 (14.31) | 6.07 (14.36) | **6.08** (14.65) |
| the palace | 4x | 23.21 (68.21) | 14.80 (54.04) | 14.69 (53.59) | 14.94 (57.51) |

- **The world with the UNORM depth costs what the GPU's depth costs**, at every scene and scale: 2.6-4.4 ms a game frame
  less than today's at 2x (48%, 42%, 35%), as `world-plain.md` measured for `gpu`. The bias, the per-pixel clamp and the
  16-bit format cost nothing measurable.
- Outside the world, the depth copy's clears and DATE's marking go too (the 2D column: 2.86 to 1.84 at 099).
- **The UNORM frame is dearer than `gpu`'s by 0.3-1.9 ms at 2x, all of it DATE drawn by a copy** (D16 has no stencil:
  each DATE draw copies its target; the work the profile does not put in the table's columns: at the pond 2.2 ms).
  That is the experiment's, not the depth's: the scene renderer's DATE is to be settled without the depth's stencil
  (a stencil image of its own, or the shadows drawn another way).
- The same holds at 1x and 4x; at 4x the sums overcount (`world-plain.md`, "What the sums mean").

## 4. The world's overdraw

Fragment shader invocations of the world's native batches (not DATE's, not the sky's) a pixel of the frame buffer, at
the owner's 2x with native widescreen (1920 x 896), the median of 34 pictures each (`depth_ties.py --scenes`); and with
the rule, the fragments it tested and passed a pixel (each fragment shaded is tested: the rule's shaders keep the early
test with the corners' bound), and how the draw order runs against Z:

| scene | invocations a pixel: rule / gpu / layers (000, 280, 700) | passed a pixel | of them by draws not decals | Z against draw order |
|---|---|---|---|---|
| 099's start | 3.13 / 3.13 / 3.13 (3.11, 0.01, 0.01) | 2.56 | 0.82 | -0.48 |
| the pond (009) | 6.25 / 5.69 / 5.69 (1.89, 0.02, 3.8-4.3) | 5.24 | 0.66 | +0.50 |
| the palace (022) | 3.78 / 3.73 / 3.73 (3.64, 0.09, 0.05) | 1.84 | 1.83 | +0.35 |

Over the tour (1x, the rule): a median of 2.13 fragments a pixel at a map (the most 4.6), 0.75 of them blended.

- **The game draws neither back to front nor front to back.** The rank correlation of its opaque draws' Z with their
  order is -0.48 at 099 (nearer first, mostly), +0.50 at the pond and +0.35 at the palace (farther first, mostly):
  the map's records in their own order, then objects and creatures.
- **Much of the overdraw is layers, which must be shaded.** At 099 the draws that are not decals pass 0.82 fragments
  a pixel and the decals (blended over their surfaces: the rock shading) 1.74: drawing front to back would save at most
  what fails the depth test, 3.13 - 2.56 = 0.6 fragments a pixel. At the pond 3.8 of 5.7 are the effects (program
  0x700: blended, no Z write); its opaque world passes 0.66. Only at the palace is the opaque world's overdraw large:
  3.64 shaded for 1.83 passed, about 1.8 fragments a pixel a front-to-back order (or a depth pre-pass) would reject.
- The rule's shaders are rejected by the early test almost as the plain ones are (3.13 against 3.13 at 099; at the
  pond 6.25 against 5.69, the effects' 0.6 more): the rule's cost is its interlock, copy and planes, not lost early
  rejection.

So the world's floor at 2x on the Vega 7 is about what one layer of map-like geometry and its layers cost
(`world-plain.md`, "The floor": 0.9 ms a full-screen layer, 0.12 hidden), and the order the scene renderer chooses
saves most at palace-like scenes.

## Checks

- **The default path is `main`'s**: the whole-game tour at 1x with native draw (760 views) and 25 pictures at 2x of five
  scenes (009, 024, 028, 099's start, 063) byte for byte the same as `main` (a2aad657) built beside it: the invariant
  positions, the counters' code in the rule's shader and the new modes change nothing unless asked for.
- ctest: 37 of 37 pass.
- Not run: the sanitizers, the Vulkan validation layer, the 9070 XT, NVIDIA (whose D16 rounding and depth clamp would
  need the same tour).

## What it means for the scene renderer

`renderer-scene-not-gs.md`'s step 1 changes: **depth by precision, not by layers.**

1. No ties to list, no interlock, no copy, no planes pass, no shader writing depth: the vertex transform gives each
   corner its Z, the GPU interpolates and rounds it into `D16_UNORM` (for the game's 16-bit Z buffers) and tests it
   with the GS's own test (GEQUAL, GREATER, ALWAYS), the depth clamp clamping each pixel to the format's largest.
2. The early depth test stays on for every opaque draw, so the scene renderer's order (opaque front to back where it
   may) rejects what is hidden before it is shaded; layers keep the stream's order after their surfaces, which the
   GS's tie rule needs (the later draw wins).
3. Left to settle in the scene renderer: DATE (the entity pool's shadows) without a stencil in the depth buffer;
   the 24- and 32-bit Z buffers (the menus') on a float depth; and the 4x pictures' rounding (each of a GS pixel's
   pixels rounds its own Z, as exact Z does today).

## Running it again

```sh
# the census (the depth rule, the default): ties and overdraw at each view
python3 tools/map_tour.py --out OUT/pc_rule --positions 3 --ties --renderer pc \
    --extra "--native-scale 1 --no-native-widescreen --native-draw on --frame-rate 30"
python3 tools/depth_ties.py OUT/pc_rule
# a mode's tour, and paraLLEl-GS's
KF4_PC_DEPTH=unorm python3 tools/map_tour.py --out OUT/pc_unorm --positions 3 --renderer pc --extra "..."
python3 tools/map_tour.py --out OUT/gs --positions 3 --renderer gs --extra "--frame-rate 30"
python3 tools/tour_compare.py OUT/pc_unorm OUT/gs --out OUT/compare_unorm
# motion, with the counters
python3 tools/motion_compare.py --out OUT/motion --profiles 1x --no-240 --ties --pc-extra "--native-draw on --frame-rate 30"
# the cost (real window)
python3 tools/world_plain.py --out OUT/cost --variants exact,gpu,layers,unorm --rounds 2
```

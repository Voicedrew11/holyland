# The world drawn plainly: what it costs

**2026-11-19, later: the plan has moved on** (`renderer-scene-not-gs.md`, "The decision"): the scene renderer is
built, today's renderer is not sped up, and depth is settled by `depth-by-layers.md` (the GS's precision, not
knowledge of the geometry). "What it means for the rewrite" below is as it was written.

2026-11-19. The step after the inventory of full-screen layers (`fullscreen-layers-inventory.md`) in
`renderer-scene-not-gs.md`, as `HANDOFF_world_plain.md` set it: what the world costs on the GPU drawn the GS's way
today, what each GS-exact mechanism costs, and what the floor is. Measurement and developers' switches only; nothing a
player gets.

Made by `tools/world_plain.py` (tools/README.md, "world_plain.py"), with `layer_inventory.py`'s live step and the
prototype plain path `KF4_PC_PLAIN_WORLD` (`pcr_canvas.cpp`). Every result is in `world-plain-results.md`; the raw
runs are the game's and stay outside the repository (the owner's `~/Documents/darker-reality-measurements/`).

## The answer, short

At the owner's settings (2x with native widescreen, 1920x896) on the Vega 7, the world costs **5.6-10.2 ms of GPU a
game frame** in the three scenes (a third to a half of the frame), and as much again or more in each picture
between at the pond and the palace.

| | 099's start | The pond (009) | The palace (022) |
|---|---|---|---|
| The world today (the depth rule, exact Z's planes) | 5.55 | 10.22 | 9.18 |
| The world with the GPU's depth test | 2.83 | 5.87 | 6.07 |
| The whole frame today | 16.07 | 20.22 | 19.06 |
| The whole frame with the GPU's depth test | 11.77 (-27%) | 14.21 (-30%) | 14.18 (-26%) |
| The world in a picture between, today / GPU's depth | 3.74 / 2.70 | 16.3 / 10.8 | 14.7 / 9.4 |

- **Exact depth is half the world**: the depth rule's interlocked test, its copy of the depth, and exact Z's planes
  pass. It costs 2.7-4.4 ms a game frame at 2x, 6-11 ms at 4x, more than all the full-screen layers together (3.3 ms
  in the inventory), and it makes DATE's set-up and the clears dearer (another 1.4 ms).
- **Nothing else that is GS-shaped costs anything in the world.** The blends done in the shader: nothing. One image
  for both buffers, the GENERAL layout, no colour compression: nothing (the world drawn into images of its own,
  optimal and compressible, costs the same; RADV's compression turned off changes nothing anywhere). Blending the
  whole world: nothing at 2x (drawn unblended, the same). Anisotropic filtering: 0-0.3 ms.
- **With the GPU's depth the world is about three full screens of shaded pixels**: one full-screen layer of map-like
  geometry costs 0.91 ms on this card at 1920x896, a layer that fails the early depth test 0.12. The floor of a world
  with each pixel shaded once is about 1-1.5 ms at 2x, a quarter of today's, if the overdraw can be rejected early (the
  draw order is not measured yet).
- **The skinned models (program 0x280)** are a fixed cost that resolution does not explain: about 890 small batches a
  game frame at the pond, 2.4 ms at 1x and 3.4 at 2x with the GPU's depth.
- **A picture between draws the world dearer than a game frame does**: 1.5-1.8 times at the pond and the palace.

So the handoff's question ("if the world is cheap drawn plainly, start from a scene renderer; if not, find what else is
in the way") has a third answer: drawn into PC-style targets the world is **not** cheaper, because targets were not
what was in the way. What is in the way is what a scene renderer would change and a GS replay cannot: exact depth
decided per pixel in draw order, a draw order the renderer may not choose, the skinned models' many small draws, and
pictures between replayed through the GS's stream. Start from the scene renderer, not from separate targets
("What it means for the rewrite", below).

## How it was measured

- **The machine**: the Ryzen 5 5625U (Vega 7, RADV RENOIR, Mesa 26.0.8), mains power, the **balanced** profile, as for
  the inventory.
- **The settings**: the owner's (`~/.config/darker-reality/settings.json` copied: FXAA, 16x anisotropic, native draw
  on, native widescreen, instant mouse look), the native scale set to 1, 2 or 4 (1280x448, 1920x896, 3840x1792 a
  buffer). A real window, as `CLAUDE.md` asks for graphics.
- **The scenes**: `layer_inventory.py`'s `start` (099 standing, from its base quick save), `pond` (009, the swamp, a
  walk) and `palace` (022, before a Watcher). Each run plays about 14 s and profiles 4 s (`--profile-at`,
  `KF4_GPU_DETAIL=1`: the GPU's time by kind of work and by shader state). The game held 30 game frames a second in
  every run.
- **The variants** (`world_plain.py --variants list`), each off the exact path, so the picture may differ:

  | Variant | What | Switches |
  |---|---|---|
  | exact | today's: the depth rule (shader interlock), shader blends, exact Z's planes | |
  | exactz | exact Z with decals settled (the path without interlock) | `KF4_PC_DEPTH=exact` |
  | nosettle | exact Z, decals not settled | `KF4_PC_DEPTH=exact KF4_PC_DECAL_SETTLE=0` |
  | noplanes | the depth rule, the native draws' Z interpolated (no planes pass) | `KF4_PC_PLANES=0` |
  | gpu | the GPU's depth test: no rule, no exact Z, no planes | `KF4_PC_DEPTH=gpu` |
  | noblend | the depth rule, the blends with no Vulkan form as the nearest | `KF4_PC_SHADER_BLEND=0` |
  | plain | the GPU's depth and the nearest blends | both |
  | opaque | the GPU's depth, the world's batches unblended | `KF4_PC_PLAIN_WORLD=opaque` |
  | plainworld | the GPU's depth, the world into images of its own (below) | `KF4_PC_PLAIN_WORLD=1` |
  | plainopaque | ... and unblended | `KF4_PC_PLAIN_WORLD=unblended` |
  | *_nodcc, *_nohiz | RADV's colour compression or HiZ off | `RADV_DEBUG=nodcc`, `nohiz` |
  | gpu_aniso1 | the GPU's depth, anisotropic filtering 1x | `native_aniso: 1` |

- **The world**: the native draws: program 0x000 (the map, objects, the entity pool's models) less its DATE draws
  (`DATE stencil`, the inventory's open question 2) and the sky (read from the scrolling texture's copy, open
  question 3); program 0x280 (the skinned models: creatures, the map's skinned objects, the arm); program 0x700 (its
  effects; at the pond the bulk of it); and exact Z's planes pass. Every figure is **GPU ms a game frame**: the run's
  GPU time over its game frames. At 60, a game frame and a picture between; the picture between's is the 60 run's
  less the 30 run's.
- **What the sums mean**: each figure is a sum of timed intervals (`pcr_gpu_time.cpp`, timestamps at
  `ALL_COMMANDS`). Within one command stream they do not overlap, but separate submissions (the planes pass, the
  scanout and present, the pump's pictures between) can run at once, and then the sums exceed the wall clock: at 4x
  every run summed 6.3-8.8 s of GPU in its 4 s, and at 60 the pond and the palace 4.1-5.9 s; at 1x and 2x at 30 none
  did (at most 2.4 s). So the ms at 4x and at 60 overcount, more where the card is busier; compare them with each
  other, not with a frame's 33 ms. The method is the inventory's live one, so its figures compare with these.
- **Rounds**: the main matrix (3 scenes, 7 variants, 3 scales) ran twice in the same order; the medians are given. The
  two rounds of a cell were within 0.25 ms at 1x and 2x (one 0.53), within 0.85 at 4x. The diagnostics at 2x ran twice,
  the plain path at 1x and 4x once.
- **The prototype plain path** (`KF4_PC_PLAIN_WORLD`, with `KF4_PC_DEPTH=gpu`): each world batch (a native program's
  but the ripple's, into a 32-bit colour surface 640 or more wide; not DATE's, not one textured from a surface, none
  blended in the shader) drawn into a colour and a depth image of its own, the size of the surface it was for, made as
  a PC game makes its targets: optimal layouts, colour or depth attachment use alone (so DCC and HiZ may be kept),
  cleared by the pass's load op (a fast clear) after each clear of the frame buffer. Same formats as the frame's
  (RGBA8, D32S8), so the same pipelines. The picture lacks the world: for timing only. It adds pass changes (7417
  passes in a run against 3310: about 13 a game frame where the world and the rest alternate).

## What the world costs, by scale

GPU ms a game frame, the world today against the GPU's depth (`exact`, `gpu`), and the whole frame:

| Scene | Scale | World today | World, GPU's depth | Frame today | Frame, GPU's depth |
|---|---|---|---|---|---|
| 099 | 1x | 3.08 | 1.63 | 9.01 | 7.34 |
| 099 | 2x | 5.55 | 2.83 | 16.07 | 11.77 |
| 099 | 4x | 17.08 | 10.68 | 66.35 | 53.51 |
| pond | 1x | 6.42 | 4.28 | 12.15 | 9.61 |
| pond | 2x | 10.22 | 5.87 | 20.22 | 14.21 |
| pond | 4x | 29.79 | 18.83 | 73.19 | 57.29 |
| palace | 1x | 6.91 | 4.93 | 12.65 | 10.22 |
| palace | 2x | 9.18 | 6.07 | 19.06 | 14.18 |
| palace | 4x | 22.29 | 14.55 | 67.70 | 52.66 |

The world's parts at 2x (today / GPU's depth): 099, program 0x000 4.61 / 2.58, 0x280 and 0x700 0.27 / 0.25, the planes
0.66 / 0; the pond 3.58 / 2.46, 5.26 / 3.41, 1.37 / 0; the palace 4.94 / 3.45, 3.15 / 2.62, 1.10 / 0. At 4x the game
still held 30 game frames a second, but the sums there overcount (above); its scanout and present sum to 22-26 ms a
game frame, more than the world (an open question below).

## What each GS-exact mechanism costs

The world at 2x, GPU ms a game frame (the medians of two rounds):

| Variant | 099 | Pond | Palace | What it says |
|---|---|---|---|---|
| exact (today) | 5.55 | 10.22 | 9.18 | |
| exactz (exact Z, decals settled) | 7.49 | 13.83 | 11.28 | the path without interlock is dearer than the rule: the settle draws decals three times |
| nosettle (exact Z alone) | 4.33 | 9.91 | 8.53 | exact Z written by the shader, with the planes: what exactness costs without ties handled |
| noplanes (the rule, Z interpolated) | 5.07 | 8.81 | 8.18 | the planes pass's 0.66-1.37 less what interpolating in the shader costs |
| gpu | 2.83 | 5.87 | 6.07 | |
| noblend (no blends in the shader) | 5.49 | 10.39 | 9.16 | nothing: the world's blends all have a Vulkan form |
| plain (gpu + noblend) | 2.82 | 5.88 | 6.08 | the same as gpu |

So, of today's world at 2x: **the depth rule's interlock and copy** 1.2 ms at 099 (exact against nosettle), 0.3 at the
pond, 0.65 at the palace; **exact Z** (the planes and the shader's depth) 1.5 at 099, 4.0 at the pond, 2.5 at the
palace (nosettle against gpu); **shader blends** nothing. Outside the world the rule costs as well: the depth copy's
clears (`clear_depth_copy`: 2D 3.02 / 1.93 at 099, 2.72 / 1.79 at the pond, 3.02 / 1.82 at the palace) and DATE's
marking of the copy (0.65 / 0.33, 0.96 / 0.60, 0.85 / 0.50), 1.4 ms a game frame together.

**The planes pass** is one submit a game frame: 0.67 ms at 099 (about 16,000 triangles a game frame), 1.35 at the
pond (23,000), 1.05 at the palace (36,000), the same at 1x and 2x and less at 4x (0.34, 1.14, 0.87). That is about 40
ns a triangle, for work that is a few multiply-adds a triangle: it runs the VU1 vertex programs again for each corner
(`pcr_planes_*.comp`) in 64-bit integers, which Vega emulates. Cheaper at 4x, where the GPU is busier: the card's
clocks are likely low at 1x and 2x, which would make every small cost look larger than it is. (The triangle counts are
the run's averages from its load: a lower bound for play.)

**Exact Z on the skinned models** is the pond's largest item: 0x280 and 0x700 5.26 ms today against 3.41 with the
GPU's depth, and 5.41 with exact Z alone. Its batches are many and small (at the pond about 890 timed intervals a game
frame for 0x280), so the cost looks like work a batch (planes room, runs cut), not a pixel.

## Targets, compression and blending

At 2x, the world (and the whole frame), GPU ms a game frame:

| Variant | 099 | Pond | Palace |
|---|---|---|---|
| gpu | 2.83 (11.77) | 5.87 (14.21) | 6.07 (14.18) |
| gpu_nodcc: compression off | 2.82 (11.75) | 5.87 (14.20) | 6.07 (14.26) |
| exact_nodcc: today's, compression off | 5.59 (16.16) | 10.30 (20.32) | 9.20 (19.30) |
| gpu_nohiz: HiZ off | 2.97 (12.46) | 6.17 (15.59) | 6.29 (15.28) |
| opaque: the world unblended | 2.82 (11.63) | 5.69 (13.91) | 6.06 (14.32) |
| plainworld: images of its own | 2.88 (11.40) | 6.01 (13.88) | 6.04 (13.61) |
| plainworld_nodcc | 2.93 (11.44) | 6.05 (13.91) | 6.13 (13.71) |
| plainopaque: its own images, unblended | 2.88 (11.31) | 5.87 (13.77) | 6.04 (13.74) |
| gpu_aniso1: anisotropic 1x | 2.55 (11.34) | 5.68 (14.03) | 6.06 (14.35) |

At 1x and 4x the plain path is the same story: 099 1.63 / 1.73 (gpu / plainworld) and 10.68 / 10.06, the pond 4.28 /
4.38 and 18.83 / 17.97, the palace 4.93 / 4.97 and 14.55 / 13.67 (one run each; 4x's rounds spread up to 0.85).

- **Colour compression does nothing here**, on the frame's surfaces or on the plain images. RENOIR is GFX9: either
  RADV keeps no DCC for these images or the world gains nothing from it. Separate, compressible targets would not make
  the world cheaper on this card.
- **HiZ works today** and is worth 0.15-0.3 ms in the world and 0.5-0.9 ms in the 2D draws (the clears).
- **Blending the whole world costs nothing at 2x** (the world's batches are drawn with `(Cs-Cd)*As+Cd` throughout).
- **The plain images' frame is 0.4-0.7 ms cheaper outside the world**: the scanout and present, the picture having no
  world in it to anti-alias. Not a saving of the plain path.
- **Texturing** (anisotropic filtering at 16x against 1x) is 0-0.3 ms.

## The floor

`kf4-layerbench --size 1920x896 --target "8 bits a channel, 1 sample"` (Vega 7; ms a full-screen layer):

| A full-screen layer of | ms |
|---|---|
| one colour, no depth, no blend | 0.21 |
| two triangles, textured, depth | 0.54 |
| two triangles, textured, depth, blended | 0.70 |
| 11,000 triangles, textured, depth, blended (map-like) | 0.91 |
| 45,000 triangles, textured, depth | 0.93 |
| hidden: fails the early depth test | 0.12 |
| depth alone, two triangles | 0.22 |

This card pays about 0.2 ms to touch every pixel of one 1920x896 target once, and about 0.9 for a map-like layer. The
world with the GPU's depth at 2x is 2.8 ms at 099 (about three such layers), 5.9 at the pond and 6.1 at the palace
(of them 2.6-3.4 ms the skinned models and effects, much of it fixed: 2.4 ms at 1x at the pond). From 2x to 4x the
static world at 099 grows by 1.5 ms a million pixels, so about 2.6 of its 2.8 ms at 2x is pixel work. A PC renderer
drawing the opaque world front to back (each pixel shaded once, the rest failing the early test) would pay about one
layer and some hidden ones: **about 1-1.5 ms for the world at 2x on this card**, plus the vertices and draws, which at
the pond's skinned models need batching to be small. How much overdraw the game's draw order has (back to front, as a
GS game blending everything would draw?) is not measured: a pipeline statistics query of fragment shader invocations
a game frame would say it.

## Pictures between

At 60 (each game frame with one picture between), the world, GPU ms:

| Scene | A game frame, today / GPU's depth | A picture between, today / GPU's depth |
|---|---|---|
| 099 | 5.55 / 2.83 | 3.74 / 2.70 |
| pond | 10.22 / 5.87 | 16.3 / 10.8 |
| palace | 9.18 / 6.07 | 14.7 / 9.4 |

At the pond the static world's unblended draws (`native 000 | sampled modulate rgb | fog | Z >= write`) cost 2.05 ms
more in a picture between against 0.48 in the game frame, and program 0x700's two states 1.4 ms more each: the kept
walk's batches (runs, merged runs, dual-source blends: `pcr_canvas.cpp`) are dearer than the frame's own draws there.
Not traced in this step. Every run at 60 showed all its vblanks' pictures and held 30 game frames a second; at the
pond and the palace its sums overcount (above), so the pictures between's figures are an upper bound, but the
comparison between the variants and with the game frame's draws by state holds.

## What it means for the rewrite

`renderer-scene-not-gs.md` ordered: fold the layers, then separate and compressible targets, then pictures between
without replays. Measured, the world changes that:

1. **Separate, compressible targets do nothing for the world on this card.** They are still worth the clears (0.47
   ms) and what folding needs (the history image), but they are not the world's lever.
2. **Exact depth is the largest single cost** (2.7-4.4 ms a game frame at 2x, plus 1.4 outside the world, all paid
   again in pictures between), and the alternatives tried before failed (`PC_RENDERER.md`: the decal nudge, the
   draw-order tie, the settled decals). They failed because a GS replay must decide ties per pixel, in the stream's
   order, with no idea what the triangles are; the one way that was right everywhere is the rule's interlocked test of
   every draw. A scene renderer is told what the triangles are, so:
   - **ties are known, not detected**: the map's data says which records are layers over which surface (the coplanar
     pairs: moss, puddles, the lava's edges, 280's fire), found once when the map loads;
   - **the order is the renderer's**: opaque geometry first, front to back (early rejection, the floor above), each
     layer after its own surface, then the see-through things. Nothing reads the depth in draw order, so no
     interlock, no copy, no copy clears, no DATE marking;
   - **depth comes from one place**: the transform that places the vertices gives each triangle's plane once (no
     second pass running the vertex programs again), and a layer drawn with its surface's plane (or tested EQUAL
     against it) ties exactly with the GPU's own GEQUAL, with no depth written by the shader.

   What it does not change: where two different surfaces cross, the GS rounds Z to whole numbers and the GPU keeps a
   fraction, so they may pick different winners along that line. Matching `main` byte for byte there still needs the
   GS's arithmetic for those pixels; elsewhere the GPU's test gives the same pixels.
3. **The skinned models need batching**: hundreds of small draws a game frame at the pond (2.4 ms at 1x with the GPU's
   depth), and the most of exact Z's cost there.
4. **Pictures between from the scene**, not from the replayed stream: they draw the world 1.5-1.8 times dearer than a
   game frame does.
5. **The final step at 4x** (scanout and present, summing to 22-26 ms a game frame) is larger than the world at 4x
   and needs its own step.

## Open questions

1. **Overdraw and the draw order.** How many times each pixel is shaded in the world, and whether the game draws back
   to front: a pipeline statistics query (fragment shader invocations) a game frame. It decides how close to the floor
   a front-to-back opaque pass gets.
2. **The planes pass at 40 ns a triangle.** Whether it is the 64-bit maths, re-running the vertex programs, or the
   GPU's clocks at low load. A scene renderer makes the planes in the transform; the current renderer could too.
3. **The GPU's clocks.** The planes pass is cheaper at 4x than at 2x: at 1x and 2x the card may not be at full clock,
   so small costs look larger than they are (`PC_OPTIMIZATION.md` measured 5-20% moves with the profile).
4. **The kept walk's world in pictures between**: why its batches cost 1.5-4 times the frame's at the pond.
5. **The scanout and present at 4x**: summing to 22-26 ms a game frame (two vblanks), the largest item at 4x in
   every scene; and which submissions overlap there, so that the sums exceed the wall clock.
6. **The 9070 XT** was not run; the ms here are the Vega 7's on the balanced profile.
7. **The intersection lines' exactness**: how many pixels of a map tour differ between exact Z and the GPU's depth
   once ties are handled by layer, against paraLLEl-GS: what "exact" would have to mean for the scene renderer's depth.

## Running it again

```sh
python3 tools/world_plain.py --out OUT --disc ISO --rounds 2
python3 tools/world_plain.py --out OUT --disc ISO --scales 2 --rounds 2 \
    --variants gpu_nodcc,exact_nodcc,gpu_nohiz,gpu_aniso1,opaque,plainworld,plainworld_nodcc,plainopaque
python3 tools/world_plain.py --out OUT --disc ISO --scales 1,4 --variants opaque,plainworld,plainopaque
python3 tools/world_plain.py --out OUT --disc ISO --scales 2 --rates 60 --variants exact,gpu
out/linux-clang/kf4-layerbench --size 1920x896 --target "8 bits a channel, 1 sample"
```

About an hour of real-window runs in all, on mains power with nothing else on the GPU.

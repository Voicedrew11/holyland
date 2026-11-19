# Drawing the scene, not replaying the GS

2026-11-19, revised three times that day: after `world-plain.md`, after `depth-by-layers.md`, and after the owner's
decision (below). A direction for the PC renderer: why it is slow for a PS2 game, and what replaces it. The
measurements behind it: `camera-whip-slowdown.md` (the game held by the GPU), `fullscreen-layers-inventory.md` (the
full-screen layers), `world-plain.md` (the world and the GS-exact mechanisms) and `depth-by-layers.md` (depth, ties,
overdraw).

## The decision (2026-11-19)

**Build the scene renderer; do not speed up today's renderer.** Today's PC renderer (`port/gs/pc/`, a replay of the
GS's stream) stays the players' renderer, fixed where it is broken, until the scene renderer replaces it; the savings
the studies found (folding the layers, the depth, the pump's copies) are built into the scene renderer, not patched
into the replay. Next: settle the scene renderer's design (`HANDOFF_scene_renderer_design.md`), then build it in
slices.

## The goal

King's Field IV is a PS2 game whose own code runs natively. Its picture is 640x448 with a few thousand triangles a
frame. On any GPU sold in the last ten years it should be **extremely fast**: at 1080p on an integrated GPU, a
picture should cost little more than drawing its triangles once, at any frame rate. Today a game frame costs 16-20
ms of GPU on the owner's Vega 7 at 2x (1920x896), and a picture between as much again; the owner's laptop should do
several hundred pictures a second there.

## Where the time goes: the GS's way of drawing, replayed

The game side is fast already: in every profile the game's thread spends 75-85% of its time waiting for the
renderer. The cost is in the PC renderer, which draws the GS's stream the way the GS drew it. Measured at 2x on the
Vega 7, GPU ms a game frame:

| What | Cost | Why it is the GS's way |
|---|---|---|
| The world (the map, objects, creatures, effects) | 5.6-10.2 | below |
| ... of it exact depth: the depth rule's interlocked test and copy, exact Z's planes pass | 2.7-4.4, and 1.4 more outside the world | the GS's integer Z and its ties, decided per pixel in draw order |
| ... the rest: about three full screens of shading, and the skinned models' many small draws | 2.8-6.1 | the GS's order (its memory made overdraw nearly free), VU1's batches |
| The full-screen layers (clears, the alpha's reset, fills, the brightness pass) | 3.3 median | the GS made a full-screen pass nearly free |
| The final picture and present | about 2.7 a vblank | separate passes |
| A picture between | as much as a game frame, or more | the whole stream replayed, the world 1.5-1.8 times dearer, surfaces copied |

What does **not** cost anything measurable (`world-plain.md`): the blends done in the shader, both frame buffers one
image, the GENERAL layout, colour compression, blending the whole world, separate PC-style targets. The cost is not
where the pixels are kept but **how the scene is drawn**: in the GS's order, with the GS's depth, as many small draws,
all of it again for each picture.

## What the scene renderer is

It reads the game's frame as a scene (meshes, instances, cameras, materials: the native draws carry most of it) and
draws it the way a PC game would, with the GS's results where they show. What the studies have settled, and what is
left for the design:

### Settled by measurement

- **Depth: the GS's precision, not knowledge of the geometry** (`depth-by-layers.md`). The ties the GS's depth makes
  cannot be listed from the data (spread over the map's records; far from the eye unrelated surfaces share a whole Z
  of the game's 16-bit Z buffer), and nothing needs to list them. A `D16_UNORM` depth buffer for the game's 16-bit Z
  buffers rounds each pixel to the GS's whole Z in the GPU's own depth unit, the depth clamp clamps each pixel to the
  format's largest, and the GPU's GEQUAL lets the later draw win a tie, as on the GS: early tests kept, no interlock,
  copy, planes or bias, no shader writing depth. Measured: the world at the GPU depth's cost (2.8 / 6.0 / 6.1 ms at
  2x at 099, the pond and the palace, against today's 5.4 / 10.3 / 9.3), the whole-game tour closer to paraLLEl-GS
  than today's depth rule. Open (the design's): whether "exact" is that (single pixels differ from the GS where its
  rounding and the GPU's fall on either side of a half) or the GS's arithmetic in the shader (a ceiling to measure);
  and DATE without the depth buffer's stencil.
- **Keep the game's draw order.** The GS's tie rule (the later draw wins) decides about 7% of the fragments (most of
  them the same plane drawn twice), so reordering draws changes pictures. Batching consecutive draws of one state into
  one call keeps their order and is exact; sorting front to back is not, and would save little: the world shades 3-6
  fragments a pixel at 2x, much of it layers and effects that must be shaded anyway; only palace-like scenes have
  large opaque overdraw (about 1.8 fragments a pixel). A depth pre-pass, if any, keeps the order.
- **The full-screen layers fold into the final pass** (`fullscreen-layers-inventory.md`): the brightness pass, tints,
  fades, the screen fill, applied with the same arithmetic in the same order; the dead writes left out; clears as fast
  clears. About 2.9 ms of a game frame at the median (folding 1.75, the dead writes 0.66, the fast clears 0.47), and
  in every picture between.
- **Targets as images of their own** cost nothing extra and are what fast clears and folding need; they are the scene
  renderer's way, not a step of their own.

### For the design (`HANDOFF_scene_renderer_design.md`)

- What a frame's scene is, made from the native draws and the GIF draws that remain (the 2D, the HUD, the menus, the
  screen passes), and where each draw's material (texture, CLUT, blend, tests) is resolved: today only in the chain's
  order, against the GS's memory as the uploads leave it.
- The targets the game draws into and reads back (the reflection, the water and lava targets, the sky's scrolling
  texture, the frame copied for the under-water and drain screens, the Dark Succubus's copy, the menus' grab).
- The skinned models' many small draws (program 0x280: about 890 a game frame at the pond) batched.
- **Pictures between from the scene**: each drawn at its time (instances and camera moved, as `part_moves` moves them
  today), not a replay of the stream with copies of the frame's surfaces; instant mouse look as a reprojection of the
  finished picture with a margin (`camera-whip-slowdown.md`).
- **The GPU never holds the game**: pictures scheduled by their measured cost, one that cannot be drawn in time left
  out, the game always at its rate (`camera-whip-slowdown.md`).
- The final picture and present (scaling, sharpening, anti-aliasing: about 2.7 ms a vblank at 2x), and a vblank that
  shows the same picture not made again.
- How it lives beside today's renderer while it is built, and how its pictures are judged.

## Order

1. **Measure** (done 2026-11-19): `camera-whip-slowdown.md`, `fullscreen-layers-inventory.md`, `world-plain.md`,
   `depth-by-layers.md`.
2. **Settle the design** (`HANDOFF_scene_renderer_design.md`): `scene-renderer-design.md`, with the owner's choices
   (depth's exactness above all) made on its numbers.
3. **Build it in slices**, as the design orders them, each a renderer the developers can run beside today's
   (`--renderer`), judged on its pictures and its GPU time.
4. **Replace** today's renderer for players once the scene renderer draws everything at least as close to
   paraLLEl-GS, and remove the replay.

Each slice is judged on its pictures (against paraLLEl-GS by `tour_compare.py` and `motion_compare.py`, no further
from it than today's renderer; byte for byte against today's where the design says a part is exact, such as the folded
layers' arithmetic) and on GPU ms a game frame and a picture between, in a real window with the owner's settings
(`world_plain.py`, `layer_inventory.py --steps live`), on the Vega 7 and the 9070 XT. The target at 2x on the Vega 7:
the world 1-2 ms a game frame, a game frame in all a small fraction of today's 16-20 ms, and a picture between less than
a game frame.

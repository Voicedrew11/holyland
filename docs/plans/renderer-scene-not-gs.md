# Drawing the scene, not replaying the GS

2026-11-19 (revised the same day after `world-plain.md`). A direction for the PC renderer: why it is slow for a PS2
game, and what it should become. The measurements behind it: `camera-whip-slowdown.md` (the game held by the GPU),
`fullscreen-layers-inventory.md` (the full-screen layers) and `world-plain.md` (the world and the GS-exact mechanisms).

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
| The final picture and present | about 2.7 a vblank | separate passes (the inventory's open question) |
| A picture between | as much as a game frame, or more | the whole stream replayed, the world 1.5-1.8 times dearer, surfaces copied |

What does **not** cost anything measurable (`world-plain.md`): the blends done in the shader, both frame buffers one
image, the GENERAL layout, colour compression, blending the whole world. The world drawn into images of its own, made
as a PC game makes its targets, costs the same. The cost is not where the pixels are kept but **how the scene is
drawn**: in the GS's order, with the GS's depth, as many small draws, all of it again for each picture.

## What it should become

Read the game's frame as a scene (meshes, instances, cameras: the native draws carry them already) and draw it the way
a PC game would. Exactness stays the rule: each step's pictures checked against `main` byte for byte
(`--dump-frames`), or against paraLLEl-GS by a stated pixel measure where a step changes what exact means, the owner
deciding (below).

### 1. Depth by knowing the geometry

**Revised by the proof of concept (`depth-by-layers.md`, 2026-11-19): depth by the GS's precision, not by layers.**
The ties are not knowable from the data (they are spread over the map's records, and far from the eye unrelated
surfaces share a whole Z of the game's 16-bit Z buffer), but nothing needs to know them: a `D16_UNORM` depth buffer
rounds each pixel to the GS's whole Z in the GPU's own depth unit, so the GS's ties are ties there and GEQUAL lets the
later draw win, with early tests, no interlock, copy, planes or bias; with each pixel's Z clamped by the depth clamp it
draws the whole-game tour closer to paraLLEl-GS than the depth rule. What follows was the plan before it.

The GS's Z is a whole number from each triangle's plane, and a later draw wins a tie (GEQUAL). The game relies on
ties: decals and layers drawn over their surface as meshes of their own (the moss on 063's wall, 028's puddle, 099's
rock shading, the lava's edges, 280's fire). A replay of the GS's stream cannot tell which triangles can tie, so every
pixel of every draw is tested GS-exactly, in draw order, which needs the interlock and a copy of the depth. Every
cheaper way tried failed (`PC_RENDERER.md`: the decal nudge, the draw-number tie, the settled decals), because each had
to guess ties from an anonymous stream.

A scene renderer knows what the triangles are:
- **ties are known, not detected**: which meshes lie on which (coplanar layers), from the map's data when the map
  loads, and from the models';
- **the order is the renderer's**: opaque geometry first and front to back, each layer after its own surface, then the
  see-through things, so nothing reads the depth in draw order: no interlock, no copy, no copy clears, no DATE
  marking;
- **depth comes from one place**: each triangle's plane from the transform that places its vertices (not a second
  pass running the vertex programs again), and a layer drawn with its surface's plane, or tested EQUAL against it,
  ties exactly under the GPU's own GEQUAL, with no depth written by a shader.

What it does not settle: where two different, non-coplanar surfaces cross, the GS rounds Z to whole numbers and the
GPU keeps a fraction, so they may pick different winners along that line. Whether that is exact enough is the owner's
decision, made on numbers: the proof of concept measures it (`HANDOFF_depth_by_layers.md`).

### 2. The world in a PC game's order, and few draws

Opaque geometry front to back, so that hidden pixels fail the early depth test (a full screen of map-like geometry
costs 0.91 ms on the Vega 7 at 2x, a hidden one 0.12: the world's floor is about 1-1.5 ms there, against 2.8-6.1 with
the GPU's depth today). The skinned models (program 0x280: hundreds of small draws a game frame at the pond, 2.4 ms at
1x) batched or instanced.

### 3. The full-screen layers folded into one pass

Collect a frame's screen-wide layers in their order (tints, fades, the brightness pass, the previous frame's blend) and
apply them together in the final pass, with the same arithmetic in the same order; leave out the dead writes
(`fullscreen-layers-inventory.md`: 1.2-1.8 ms of every game frame and every picture between, and 0.66 of dead
writes). Independent of 1 and 2, exact by construction.

### 4. Pictures between from the scene

A picture between draws the scene at its time (instances and camera moved), not a replay of the stream with copies of
the frame's surfaces. Instant mouse look's turn: a reprojection of the finished picture with a margin.

### 5. The final picture and present

The scaling, sharpening and anti-aliasing passes into the window, about 2.7 ms a vblank at 2x (more at 4x): the
largest cost left once the rest is done, and a limit on pictures a second by itself.

### 6. The GPU never holds the game

Pictures scheduled by measured cost (`camera-whip-slowdown.md`): a picture that cannot be drawn in time is left out,
the game always at its own rate. A safety net, first.

### Demoted: separate, compressible targets

Each displayed buffer an image of its own, optimal layouts, compression. Measured, it does nothing for the world on the
Vega 7 (`world-plain.md`). It is still worth the swap's clear as a fast clear (0.47 ms) and what folding needs (a
history image of the last picture), so it comes where those need it, not as a step of its own.

## Order, and how each step is judged

1. **The proof of concept for depth by layers** (`HANDOFF_depth_by_layers.md`; done: `depth-by-layers.md`): whether
   the game's data tells which surfaces tie (no), and what the GPU's depth costs and changes in pixels against
   paraLLEl-GS in a map tour (with a 16-bit UNORM depth, the GPU's depth's cost and fewer differences than today's);
   and the world's overdraw. It decides 1's exactness and whether the scene renderer starts there.
2. The budget-based cut (6), alongside, first.
3. Fold the layers (3) and leave out the frame's dead writes: exact, independent of the rest.
4. The scene renderer's world: depth by knowing the geometry (1), its order and draws (2).
5. Pictures between from the scene (4).
6. The final picture and present (5).

Each step is judged on its pictures (byte for byte against `main`, or the measure the owner chose for depth) and on
GPU ms a game frame and a picture between before and after, in a real window with the owner's settings
(`world_plain.py`, `layer_inventory.py --steps live`), on the Vega 7 and the 9070 XT. The target at 2x on the Vega
7: the world 1-2 ms a game frame, a game frame in all a small fraction of today's 16-20 ms.

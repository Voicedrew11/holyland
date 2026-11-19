# Drawing the scene, not replaying the GS

2026-11-19. A direction for the PC renderer: why it is slow for a PS2 game, and what it should become. The
measurements behind it are in `camera-whip-slowdown.md` (same folder).

## The goal

King's Field IV is a PS2 game whose own code runs natively. Its picture is 640x448 with a few thousand triangles a
frame. On any GPU sold in the last ten years it should be **extremely fast**: at 1080p on an integrated GPU, a
picture should cost little more than drawing its triangles once, at any frame rate. Today one game frame costs about
15.7 ms of GPU on a Vega 7 at 1920x896, and at 60 fps the game itself slows down.

## Where the time goes: the GS's way of drawing, replayed

The game side is fast already. In every profile the game's thread spends 75-85% of its time waiting for the
renderer. The cost is in the PC renderer, which still draws the way the PS2's Graphics Synthesizer did:

- **The GS made full-screen layers almost free.** Its embedded DRAM gave 48 GB/s for a 640x448 picture, so the game
  draws many layers over the whole screen: clears of colour and Z, Z-only writes, the destination alpha's reset,
  tints, fades, the brightness pass, the previous frame blended in. On a PC GPU each is a pass over the whole target
  in ordinary memory, at several times the pixels. At 2x with native widescreen that is 6 times the PS2's pixels,
  with much less bandwidth a pixel than the GS had on any integrated GPU and on many discrete ones.
- **The targets are kept the way GS memory is laid out**, which rules out the GPU's own shortcuts. Both of the frame's
  buffers are one image (640x928 GS pixels), so no clear covers a whole image and none is a fast clear. Images stay in
  the GENERAL layout and are read as input attachments and copies while they are drawn into, which keeps colour
  compression (DCC) off on many GPUs. Depth is D32 with a stencil for DATE. Every layer costs its full memory traffic:
  about 0.5 ms a layer on the Vega 7. A 9070 XT is far faster but pays the same way (`PC_OPTIMIZATION.md`).
- **Every displayed picture replays the whole GS stream.** A picture between (`--frame-rate` above 30) and instant
  mouse look's redraw both draw the frame again, layers and all. The pump also copies whole two-buffer surfaces, colour
  and depth, for each game frame it takes. At 60 with the mouse moving, that is three full draws for two vblanks.

So most of the GPU's time is memory traffic that the GS needed and a modern GPU doesn't.

## What it should become

Read the GS stream as a description of a scene, and draw that scene the way a PC game would. Exactness stays the
rule: every picture byte for byte the same as today's (`--dump-frames`), so each step can be checked against main.

### 1. Separate, compressible targets

Each displayed buffer gets an image of its own, and the reflection's and the scrolling textures' targets keep theirs.
Images go to optimal layouts per use (attachment, sampled, transfer), so the game's clear of the frame becomes a fast
clear of a whole image and drawing gets compression. A layer that reads the frame (the brightness pass, the previous
frame blended in) reads a copy or a resolved view, not the image being drawn into. This is the largest change: the
canvas's surfaces, its copies, DATE's set-up and the pump all assume the one GS-memory-shaped image.

### 2. The full-screen layers folded into one pass

Collect a frame's screen-wide layers in their order: tints, fades, the brightness pass and the previous frame's blend.
Apply them together as one shader in the scanout pass, with the same arithmetic in the same order, instead of each as
a trip through memory. Leave out what is dead: Z written that nothing reads before the next clear (the pump already
does this for pictures between, `pcr_dead_writes.h`; kf4-pc's frame doesn't yet). The first job is an inventory: every
full-screen layer in every map (a map tour with `KF4_GPU_DETAIL=2`), what it is in the game's code, and whether it can
be folded exactly.

### 3. The scene drawn once a displayed picture

A picture between should cost the world's triangles and the folded pass, not a replay of the stream with copies.
Instant mouse look's turn should be a reprojection of the finished picture in the scanout pass (a rotation needs no
new geometry), with a margin drawn past the screen's edges. With 1 and 2 done, a replay's extra cost is mostly gone
anyway; what remains is to draw only the passes a picture between changes.

### 4. The GPU never holds the game

Pictures are scheduled by measured cost: the renderer's GPU timestamps predict what a game frame and a picture
between cost, and a picture that doesn't fit its vblank is left out. The game always runs at its own rate. This
belongs here as a safety net, but it is also the first fix in the other document, since it is what the player feels
most.

## Order, and how each step is judged

1. The inventory of full-screen layers (step 2's first job): small, and it decides how much 2 and 1 are worth.
2. Fold the layers (step 2) and leave out the frame's dead writes.
3. Separate, compressible targets (step 1).
4. Pictures between and mouse look without replays (step 3).

The budget-based cut (step 4) runs alongside, first, as in `camera-whip-slowdown.md`.

Each step is judged on pictures byte for byte against main (AA off, SMAA, 1x, 2x and 4x, a map tour) and GPU ms a
picture before and after, on an integrated and a discrete GPU, in a real window with the owner's settings. The
target is a game frame at 1080p on the Vega 7 costing a small fraction of its 15.7 ms today. The exact number to aim
for comes from the inventory: what the world's triangles alone cost once the layers' traffic is gone.

# The camera-whip slowdown: what it is, and the general fixes

2026-11-19. Measured on the Ryzen 5 5625U laptop (Vega 7 integrated graphics, RADV), main at e05cd0ef. The laptop
is only where the problem is easiest to see. Nothing below is a fix for one GPU: each item is work the renderer does
on every GPU, and it is visible here because this GPU has the least headroom.

## The symptom

Whipping the camera around with the mouse makes the frame rate collapse. The profiler (Shift+P) puts **"dma GS
vsync"** at the top. Turning vsync on or off makes no difference.

## What "dma GS vsync" is

It has nothing to do with the display's vsync. It is the zone around `port_gs_run_vsync` (`port/platform/gs.c`),
which the DMA controller's thread (`kf4-dma`, `port/platform/dmac.c`) runs at each of the PS2's vblanks. It hands the
vblank's picture to the PC renderer, and at frame rates above 30 it also hands over the pictures between.

Sampled with `--profile-at` while the camera turned, about 80% of the zone's time (40% of all the DMA thread's
samples) was one wait:

    present_frames > gsr_picture > Inbox::wait_behind_at_most > condition_variable::wait

That wait (`port/gs/pc/pcr.cpp`, `picture()`) holds the sender until the renderer is at most two vblanks behind. It
had no zone of its own, so the frame profiler counted it as the zone's own work. While it waits, the DMA thread
doesn't take the game's DMA, and the game's thread waits in `port_dmac_wait` (75-85% of its time in these runs). **A
GPU that can't keep up slows the game itself down**, not just the pictures shown.

Fixed so far (uncommitted): `pcr.cpp` gives the three waits in `Inbox` wait zones of their own ("pc inbox: the
sender waits for the renderer (2 vblanks behind)", "... waits for its picture", "... waits for room"). Not yet
checked in the profiler window.

## Measurements

Every run: a real window, the owner's settings (`KF4_READ_SETTINGS=1`, a copy of the data folder), the New Game
start (map 099), `--invincible --no-audio --saves none --stats`, and a 4 s `--profile-at` window. The camera turned
by `KF4_DIAG_SPIN=6` (a simulated mouse turning at 6 rad/s, which goes through instant mouse look the way a real
mouse does), unless noted. The owner's settings that matter here: frame rate 60, resolution auto (2x; with native
widescreen the canvas is 1920x896 a buffer, 1920x1856 for the frame's two buffers), FXAA, anisotropic 16, scaling
"best" with sharpening 0.5, instant mouse look on.

| Run | GPU ms a vblank | Pictures/s, game frames/s (last half second) |
|---|---|---|
| Standing still (stick turns later, not in the window) | 17.9 | 27.0, 27.0 |
| Turning with the right stick (no instant mouse look) | 18.0 | 55.4, 27.7 |
| Mouse spin, the owner's settings | 22.5 | 21.0, 21.0 |
| Mouse spin, instant mouse look off | 16.5 | 55.1, 27.5 |
| Mouse spin, frame rate 30 | 7.9 | 60.0, 30.0 |
| Mouse spin, anisotropic 4 and 2D at PS2 scale | 22.2 | 21.5, 21.5 |
| Mouse spin, resolution 1x (1280x928) | 10.7 | 60.0, 30.0 |

The 60 fps budget is 16.7 ms a vblank. At 30 fps one game frame costs about **15.7 ms** of GPU across its two
vblanks. At 60 each game frame needs that plus a picture between. With instant mouse look and the mouse moving, it
also needs the game frame's own vblank drawn again, turned to the newest mouse reading: three full draws for two
vblanks.

### One game frame, by shader state (frame rate 30, `KF4_GPU_DETAIL=1`)

| ms a game frame | What (the identification is tentative where marked ?) |
|---|---|
| 2.12 | native 000, the map, blended (`sampled modulate rgb`, `(Cs-Cd)*As+Cd`) |
| 1.26 | `2d untextured, Z always write`: the frame's full-screen clears |
| 1.12 | native 000, `sampled modulate rgba`, blended |
| 0.87 | `2d surface copy, Z always`: a full-screen pass reading a copy of the frame (the brightness pass?) |
| 0.83 | `native 000 surface copy, blended, Z always write`: a full-screen blend of a copy of the frame (the previous frame blended in?) |
| 0.66 | `native 000 untextured, FIX blend, DATE stencil`: the destination alpha's reset strips |
| 0.53 | `2d untextured, As blend, Z always write`: a full-screen tint? |
| 0.48 | native 280 |
| 0.42 | `2d untextured, FIX blend, Z always write`: a full-screen tint? |
| 0.40 | native 000, opaque |
| 5.1 | scanout and FXAA ("picture") and scaling with sharpening ("present"): about 1.25 each, **every vblank**, two vblanks a game frame |
| 0.6 | texture copy (the copies the full-screen passes read) |

So the game frame's own draw is about 10.6 ms, and picture and present about 5.1.

About **4.6 ms of each game frame is full-screen layers** at 0.4-0.65 ms each. A layer costs the same whether it is
drawn or cleared (2026-11-19, `PC_OPTIMIZATION.md`): it is memory traffic, about 16 bytes a pixel read and written
with no compression, since both buffers are one image in the GENERAL layout.

## The four problems (general, every GPU)

### 1. A GPU that falls behind slows the game instead of the pictures

The renderer's back-pressure (`wait_behind_at_most`) holds the DMA thread, and with it the game. The adaptive cut
(`port_gs_run_vsync`: `interp_allow`, `interp_rest`) is meant to drop pictures between first. It acts on the backlog
after the fact: it cuts after two vblanks behind, waits, raises again after a rest of 30-120 vblanks, falls behind
again. In the mouse-spin run the last half second showed **0 pictures between and still only 21 game frames a
second**, so the cut alone does not keep the game at 30. **Not yet understood why.** The first thing to find out.

### 2. Instant mouse look draws the whole scene a third time

`port_interp_picture` at phase 0 (`frame_interp.c`) returns early only when the mouse hasn't moved (`!led &&
!hand_moved_since`). While it moves, the vblank's own picture is drawn again by the pump, turned to the mouse's
newest reading, after kf4-pc has already drawn the game frame. That is a full redraw for a turn of a few
milliseconds' worth of mouse movement, and it is why a mouse whip is the worst case (a stick turn doesn't do it).
The adaptive cut counts only pictures with a phase above 0, so it never drops this one.

### 3. A picture between costs as much as a game frame, or more

The pump replays the frame's whole GS stream, with every full-screen layer, for each picture between. It also copies
the frame's surfaces for each game frame it takes (`copy_surfaces`, "copy" in the GPU times: about 1.4 ms a vblank
at 60). The colour and depth are copied whole: both buffers of the two-buffer image, though the pictures between read
one. From the runs above, a picture between costs about 17 ms against about 10.7 for the game frame's draw.

### 4. A vblank that shows the same picture makes it again

At 30 (and on any vblank whose picture is unchanged), scanout, anti-aliasing, scaling and sharpening are done again
for an identical image: about 2.5 ms a game frame.

## The fixes, in order

1. **A cut by budget, and the game never held.** The renderer already has GPU timestamps for each kind of work. Use
   them to predict what a game frame and a picture between cost, and schedule only the pictures between (and mouse
   look redraws) that fit the vblank. Leave out a picture that won't fit, never hold up the game for it. First find
   why the current cut leaves the game at 21 with no pictures between (problem 1). Acceptance: under any load the
   game runs at its own rate (30 game frames a second in play), and only the number of pictures between changes.
2. **Instant mouse look by reprojection.** A pure camera turn needs no new geometry: rotate the finished picture to
   the newest mouse reading in the scanout pass (a homography, as VR's timewarp does), with the gap at the edges
   covered by drawing a small margin past the screen. That replaces a full redraw with almost nothing. To check: how
   large the lead's turn gets in a fast whip (the margin it needs), and parallax from the camera's offset from the
   turn's centre.
3. **The pump's copies cut to what is read.** Copy the buffer (rows) the pictures between read, not the whole
   two-buffer surface, and leave out the dead full-screen writes in the game's own frame as the pump does in the
   pictures between (`pcr_dead_writes.h`). Problem 3's full answer is the other document's step 3.
4. **A repeated vblank shows the picture already made.** Keep the presented image and present it again; make it again
   only when something it depends on changed (a new frame, the overlay, settings).

Every change must keep `--dump-frames` byte for byte against main (AA off, SMAA, 1x/2x/4x, several maps) and pass
a run in a real window.

## How to reproduce

```sh
S=<scratch folder>; DISC=<the disc image>
mkdir -p $S/data && cp ~/.config/darker-reality/{settings.json,pc_pipeline*.bin} $S/data/
sed -n '1,/player has control/p' tests/twin_stick.pad | grep -v "player has control" > $S/menu.pad
KF4_DIAG_SPIN=6 KF4_READ_SETTINGS=1 KF4_DATA_DIR=$S/data out/linux-clang/kf4 --disc "$DISC" --input $S/menu.pad \
    --invincible --no-audio --saves none --stats --frames 2400 --profile-at 1800:4:$S/spin.prof
python3 tools/sampler/stacks.py $S/spin.prof out/linux-clang/kf4 --gpu          # add KF4_GPU_DETAIL=1 to the run
python3 tools/sampler/stacks.py $S/spin.prof out/linux-clang/kf4 --thread kf4-dma --callees port_gs_run_vsync
```

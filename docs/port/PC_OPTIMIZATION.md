# PC renderer: where the time and the GPU memory go (2026-11-13)

A profile of the PC renderer as the owner plays it, written for whoever takes the speed work next. The owner's aim:
this game should run at about 800 pictures a second on this machine and hold little GPU memory. It reaches about 215
and holds 6 GB.

Read with `PC_RENDERER.md` (the renderer and its history), `PROFILING.md` (the tools) and `HANDOFF_2026-11-13.md` (the
GPU hang, not in the repo: ask the owner).

## The short of it

- At the owner's settings the **GPU is the limit**: 3.8 ms a picture, a ceiling near 260 pictures a second; about 215
  reached.
- About 2.3 of the 3.8 ms is **pixel work at 4x** (it goes away at 1x). The surfaces are 3840x3712.
- 0.6 ms of **"native 280" draws does not shrink at any resolution**: about 1000 tiny draws a picture, alternating
  between two blend states.
- **Presenting costs 0.4 ms** whatever the resolution.
- Even at 1x the floor is 1.5 ms: about 670 a second. 800 is out of reach at any setting until the two fixed costs
  above come down.
- The **CPU would cap it near 620 a second** once the GPU did not: the pump thread spends 1.6 ms a picture, most of
  it issuing one draw call a batch.
- **GPU memory is 6 GB** at most in play, three quarters of it screen-sized surfaces held three times over.
- The **GPU hang is open**. It was not reproduced in any run here.

## How this was measured

Machine: Linux, RX 9070 XT (RADV, Mesa 26.2.3), kernel 7.2.7, a 2560x1440 170 Hz screen.

Settings (the owner's `settings.json`): 4x native scale, native widescreen, MSAA 2, colour high, anisotropic 16,
cut-out AA ssaa, native draw on, frame rate unlocked with a cap of 500, vsync off, tearing on, full screen, scaling
best, sharpen 1.

Place: map 009, place 0, standing where the teleport puts the player. One map, and each setup run once: treat
differences under about 5% as noise.

Each run is a real full-screen window on the screen, with the owner's settings read from a copy of the data folder:

```sh
D=~/.cache/kf4hang/data    # a copy of ~/.config/holyland: settings.json and the pipeline cache
KF4_DATA_DIR=$D KF4_READ_SETTINGS=1 KF4_GSN_GPU_TIME=1 out/linux-clang/kf4 --disc <image> --no-audio --memcard none \
    --input tests/newgame.pad --teleport 1900:9:0 --invincible --frames 4700 --stats \
    [--profile-at 2700:20:FILE]        # with KF4_GPU_DETAIL=1 for the split by shader state
```

- `KF4_READ_SETTINGS=1` is new (below): a run with a pad script otherwise ignores `settings.json`.
- GPU time: the `[stats] pc renderer GPU time` line, and `tools/sampler/stacks.py FILE out/linux-clang/kf4 --gpu`.
- CPU time: `stacks.py FILE EXE --threads` and `--thread kf4-pc-pump`.
- GPU memory: the process's own, from amdgpu's fdinfo (`drm-memory-vram`), read each 0.25 s.
- Run a watchdog that kills the game when the card's memory passes 9 GB, and check `journalctl -k` for
  `ring gfx_0.0.0 timeout` after every run.

**Headless runs mislead here.** Headless at a simulated 240 Hz showed 3.4 ms a picture and 3.7 GB; they size the
flash recorder for 60 pictures a second and do not run unlocked. A hidden window (`KF4_OVERLAY_SHOT`) gives the same
GPU times as a visible one (3.799 against 3.800 ms), but the owner cannot see the run.

## The GPU's time a picture

By kind of work, at the owner's settings and with only the scale changed to 1x:

| Kind | ms at 4x | ms at 1x | What it is |
|---|---|---|---|
| 2d | 0.89 | 0.14 | the GS's own sprites and triangles: screen fills, fades, tints, copies |
| native 000 | 0.79 | 0.25 | native draw's program 000: mostly the map |
| native 280 | 0.59 | 0.61 | native draw's program 280 |
| native 700 | 0.45 | 0.06 | native draw's program 700: two blended layers |
| present | 0.47 | 0.36 | scaling to the window, sharpening, the flash recorder's blit |
| copy | 0.30 | 0.02 | the surfaces copied each game frame for the pump (`Canvas::copy_surfaces`) |
| texture copy | 0.20 | 0.03 | textures made from surfaces |
| picture, surface, upload | 0.10 | 0.03 | |
| **all** | **3.80** | **1.50** | |

By shader state (a 20 s profile, 4295 pictures, 4.2 ms a picture with the timing's own cost; the rows over 0.05 ms):

| ms a picture | Timed intervals | State |
|---|---|---|
| 0.535 | 2159489 | native 280, sampled modulate rgb, fog, blend (Cs-Cd)*As+Cd, Z >= write |
| 0.350 | 47170 | native 700, sampled modulate rgba, blend (Cs-0)*As+Cd, Z >= |
| 0.349 | 87809 | native 000, sampled modulate rgb, fog, blend (Cs-Cd)*As+Cd, Z >= write |
| 0.339 | 49271 | native 700, sampled modulate rgba, blend (0-Cs)*As+Cd, Z >= |
| 0.330 | 2223249 | native 280, sampled modulate rgba, fog, blend (Cd-0)*As+Cs, Z >= write |
| 0.319 | 15234 | 2d, untextured, Z always write |
| 0.197 | 4879 | 2d, untextured, blend (Cs-0)*FIX+Cd, Z always write |
| 0.196 | 4879 | 2d, surface copy modulate rgb, Z always |
| 0.118 | 4879 | native 000, untextured, blend (Cs-Cd)*FIX+Cd, DATE stencil, Z >= write |
| 0.106 | 4879 | 2d, sampled modulate rgba, blend (Cs-Cd)*As+Cd, DATE stencil, Z always write |
| 0.099 | 72637 | native 000, sampled modulate rgb, fog, Z >= write |
| 0.098 | 290942 | native 280, sampled modulate rgba, fog, blend (Cs-Cd)*As+Cd, Z >= write |

An interval is one stretch of draws in one state, so the two native 280 rows with 2.2 million intervals each are about
500 switches each a picture between the two states.

What each setting costs (one setting changed from the owner's; the two middle rows are from hidden-window runs):

| Setup | ms a picture | Most GPU memory |
|---|---|---|
| the owner's | 3.80 | 6.0 GB |
| MSAA off | 3.24 | 4.8 GB |
| colour exact, not high | 3.24 | 5.1 GB |
| scale 1x | 1.50 (reached the cap of 500) | 2.5 GB |

## The CPU

20 s of play, by thread:

| Thread | CPU time | What it does there |
|---|---|---|
| kf4-pc-pump | 6.9 s (34% of a core; 1.6 ms a picture) | draws the pictures between from the kept walk |
| kf4-pc | 6.3 s (32%) | reads the game's GIF packets, draws the game's frames, uploads textures |
| kf4-dma | 3.9 s (19%) | DMA, VIF1, VU1 |
| kf4 | 3.7 s (18%) | the game |
| kf4-between | 1.8 s (9%) | paces the pictures between |

The pump thread's time: 72% in `draw_batch` (34% of the thread inside `radv_CmdDraw`, 23% in
`radv_emit_all_graphics_states`), 15% in the submit's ioctl. About 1000 batches and 1200 native draw calls a picture.

No thread is the limit today. At 1.6 ms a picture the pump thread alone would cap the game near 620 pictures a second.

Of the pictures between, about 1 in 10 drawn is not shown (made too late): 1636 of 16850 in one run. Far more are left
out before being drawn (19072), which costs little.

## The bottlenecks, largest first

1. **Pixel work at 4x: about 2.3 ms.** Everything that fills or copies whole surfaces. The frame buffers' surface is
   3840x3712 (both of the game's buffers in one, at 16 bits a channel, with a two-sample image). The full-screen 2d
   passes are about 0.9 ms by themselves (the fill with Z write 0.32, the FIX blend 0.20, the surface copy 0.20, the
   DATE passes 0.22), the same passes `HANDOFF_2026-11-13.md` item 4 names. The surface copies for the pump add 0.30
   and the texture copies 0.20. To look at: whether each pass needs the whole surface or only the buffer shown, and
   whether the pictures between need them drawn again at all.
2. **Native 280: 0.6 ms that no setting lowers.** Not pixel work: the same at 1x. The draws alternate between two blend
   states about 500 times each a picture, so each draw is tiny. To look at: why two states interleave (two materials
   of one model drawn piece by piece?), and whether they can be sorted or drawn as one state. Indirect draws (#139)
   were made for this and are off on RADV since they hung the card there.
3. **Present: 0.4 ms, fixed.** Scaling "best" and sharpening to 2560x1440, and the flash recorder's blit, every
   picture. A third of an 800-a-second budget (1.25 ms) alone.
4. **Native 700: 0.45 ms** for about 11 draws a picture in two blend states: blended layers covering much of the
   screen. Pixel work (0.06 ms at 1x).
5. **The CPU's draw calls: 1.6 ms a picture on the pump thread.** Not the limit yet; the next one after the GPU.
6. **Pictures drawn and not shown: about 10%** of the pictures between.

What 800 a second needs: 1.25 ms a picture on the GPU and on the pump thread both. Today's floor with every pixel cost
gone (1x) is 1.5 ms on the GPU, of which native 280 and present are 1.0 ms.

## GPU memory

| | Most held |
|---|---|
| the owner's settings, in play at 009 | 6.0 GB (typically 5.3 GB) |
| the same before the flash recorder was held to 512 MB | past 5.5 GB in the opening; the owner's frozen game held 8 GB |
| scale 1x | 2.5 GB |

What it is made of at the owner's settings:

- **Surfaces, three sets of about 700 MB**: kf4-pc's, the pump's copy, and a spare. One set: the frame buffers'
  surface 3840x3712 RGBA16 with its two-sample image, 342 MB; two depth surfaces of that size with two samples, about
  140 MB each; small ones.
- **The flash recorder's ring** (F8: the last 2 seconds of pictures): 0.55 GB now. It was sized by the pictures a
  second: 225 MB at 60, 2.5 GB at the cap of 500.
- The rest (textures, pipelines, the swapchain, Granite's buffers) was not split up.

To look at: copying only the rows of the buffer shown, or only the surfaces the kept walk draws on; a frame buffer
surface that does not hold both of the game's buffers at full size.

## The GPU hang

Open. `ring gfx_0.0.0 timeout`, then a reset of the whole card, the game frozen after it (Granite waits forever).

- Indirect draws (#139) made it frequent in short headless runs at a simulated 240 Hz: 4 of 6 runs with them, none of
  11 without. They are off on RADV now (`KF4_PC_INDIRECT=1` turns them on).
- That is not the whole cause: at 20:33 the owner's real play hung with them off, 9 minutes in, thread kf4-pc, 3 minutes
  after native draw's kinds were turned from all off to all on in the overlay.
- None of the real-window runs here hung, but the longest was 150 s.
- The hangs have come with the GPU flat out. Lowering the work a picture may make them rarer without being a fix.

## What changed on 2026-11-13

In PR #144 (draft): indirect draws off on RADV; the surface copies' images kept and used again (`pcr_spares.h`), which
lowers the most memory held and raises the typical.

Added to the same PR after it:

- `port/platform/flash_ring.h`: the flash recorder's ring held to 512 MB (smaller pictures first, 270 rows at least,
  then fewer). Unit test `flash_ring`.
- `KF4_READ_SETTINGS=1` (`port/overlay/settings.c`): a run with a pad script reads `settings.json`, in a window on
  the screen.
- `CLAUDE.md`: graphics are tested in a real window.

Checks on those: the linux-clang build, all 19 unit tests, and `flash_ring_test` under ASan and UBSan. Not run on
them: the game under the sanitizers, a picture comparison, the Windows and gcc builds.

## What changed on 2026-11-14: GPU memory

Measured as above (a real full-screen window, the owner's settings, the old and the new build one after the other).

| | before | after |
|---|---|---|
| GPU memory at 009, most / typical | 6.1 GB / 5.2 GB | 4.0 GB / 3.8 GB |
| GPU memory at 020, most / typical | 5.8 GB / 5.0 GB | 4.0 GB / 3.8 GB |
| GPU memory at 105, most / typical | 5.7 GB / 5.0 GB | 4.0 GB / 3.8 GB |
| GPU ms a picture at 009 (45 s runs) | 3.85 | 3.71 |
| GPU ms a picture at 020 | 2.53 | 2.46 |
| GPU ms a picture at 105 | 2.42 | 2.35 |

The speed is nearly as it was: this was memory work. What the memory was (a count of every live image by size, a
temporary patch in Granite's `Image` and `Buffer`, not kept):

1. **A new 96 MB image for every picture.** The frame's last pass draws the screen over itself (the game's
   brightness: colour 136 on a 128 scale), reading a copy of the buffer. `surface_texture` made that copy's image new
   each time, the texture's size (1024x512 at 6x4: 6144x2048 at 16 bits a channel), cleared it whole, used it once and
   let it go: about 300 a second, 1.2 TB made in a 45 s run, and the game's memory swinging by 0.8 GB as Granite's
   frame contexts freed them. Now `pcr_copy_pool.h`: the copies' images are used again, and cleared only when the
   rectangle copied does not cover what the image held. Unit test `pcr_copy_pool`. `KF4_PC_COPY_POOL=0`: as before.
2. **The scanout ring: 1 GB in 16 slots.** Each slot held the picture whole (3840x1792, 28 MB), the picture at the
   window's size and a work image for the sharpening (16 MB each). The presenter only ever read the window-size one.
   A slot now holds one picture (the window-size one when the presenter asks for it, else the whole one), and the
   work image is one for all (`PictureOut`). The copy of the whole picture into the slot is gone too (picture 0.077
   to 0.056 ms).
3. **Z buffers from the boot and the menus kept for the whole of play.** A depth surface at page 140 (Z32) and one at
   page 280 of another width stayed after the last draw that used them: 141 MB each at 4x with MSAA 2, in each of the
   three sets, and copied for the pump every game frame. A depth surface not drawn with for 600 vblanks is now let go
   (`let_go_of_idle_depth`).

`--stats` now lists each surface (`[stats] pc canvas surface: ...`) and the depth surfaces let go.

What the 3.8 GB left is: the three sets of surfaces, 1.55 GB (the frame buffers' surface is still both buffers in one
image, 928 rows, with a depth surface as tall: twice what a 448-row buffer needs); the flash recorder's ring, 0.5 GB;
the scanout ring, 0.3 GB; the two surface texture copies, 0.2 GB (6144x2048 each though 3840x1792 of it is used: the
shader reads zeros past the copy, so the image is the texture's size); Granite's allocator's free blocks and slack,
0.5 GB; the rest the presenter's and the driver's.

Checks: the linux-clang build and all 20 unit tests; pictures byte for byte against the build before (52 pictures each
at 009:0, 009:2 and 020:0, headless at 240, with the pictures between); the game under ASan and UBSan in a real
window (the known reports only: `SoundDriver_Remote`, the decomp's casts, Granite's `render_pass.cpp:52`); real-window
runs of 150 s at 009 and 45 s at 020 and 105, no GPU hang. Not run: TSan, the gcc and Windows builds, a map tour
against `--renderer gs`.

### Found and not done (the speed)

- **Every map pays about 1.7 ms a picture for full-screen work** whatever is on screen (020: 2d 0.77, present 0.46,
  copy 0.26, texture copy 0.13 of 2.46 ms). At 4x with MSAA 2 and 16 bits a channel a full-screen pass touches about
  110 MB.
- **The pictures between draw the frame's clears as rectangles through the shader**: `as_clear` refuses inside a
  picture between, so three in four pictures do not use `vkCmdClearAttachments` (the "2d, untextured, Z always write"
  row, 0.32 ms).
- **The scaling shader reads about 49 picture pixels for each of the window's** (a cubic 7 x 7 at 4x into 1440 rows):
  0.43 of present's 0.47 ms. Decoding sRGB by the sampler in place of `pow()` saved only 0.05 ms (tried, taken out: a
  window's picture cannot be compared between runs to prove it exact). Two passes, across then down, would read 14.
- **The surfaces copied for the pump** (0.26 to 0.31 ms): the walk's first draw clears the buffer drawn and its Z, so
  those rows and the Z buffer need not be copied; only what the walk reads does. Needs the walk looked at before the
  copy.
- **The brightness pass** (0.2 ms and its copy 0.16): the screen drawn over itself times 136/128, not an identity, so
  it cannot be left out; it could be folded into the scanout, not byte for byte.

## Not known

- Any map but 009, and any place with creatures on screen.
- Why native 280's two states interleave.
- What present's 0.4 ms is made of (scaling, sharpening, the recorder's blit).
- What the 1.5 GB of GPU memory outside the surfaces and the recorder is.
- Whether the GPU hang needs the GPU flat out.
- The profiles are in `~/.cache/kf4hang/` on the owner's machine (`prof1.txt`, `prof2.txt` with shader states); they
  are tied to the build that made them.

## Appendix: the plan for the speed (2026-11-14)

The owner's aim stands: this game should reach very high frame rates on this machine with the settings fully up. The
work so far trims the design; this plan changes it. The owner agreed to it on 2026-11-14, including that the final
brightness step need no longer be byte for byte the reference renderer's (Phase 2).

### What the uncapped runs showed

Real full-screen window, the owner's settings, `--frame-rate-cap 0` (the cap of 500 hides the ceiling at 1x), 30 s of
play each, the scale the only setting changed. Pictures a second are from the run's totals over about 37 s, not a
counter: treat them as within 10%.

| Setup | Pictures a second | GPU ms a picture | The limit |
|---|---|---|---|
| 4x, 009 | about 300 | 3.69 | the GPU |
| 4x, 020 | about 490 | 2.52 | the GPU |
| 1x, 009 | about 790 | 1.21 | the CPU: the pump thread, 1.46 ms a picture |
| 1x, 020 | about 1150 (955 in the last half second) | 0.75 | the GPU |

By kind, GPU ms a picture:

| Kind | 4x 009 | 4x 020 | 1x 009 | 1x 020 |
|---|---|---|---|---|
| 2d | 0.90 | 0.78 | 0.12 | 0.10 |
| native 000 | 0.83 | 0.66 | 0.21 | 0.17 |
| native 280 | 0.57 | 0.14 | 0.49 | 0.12 |
| native 700 | 0.40 | 0.01 | 0.05 | 0.01 |
| present | 0.47 | 0.46 | 0.30 | 0.31 |
| copy | 0.28 | 0.26 | 0.02 | 0.01 |
| texture copy | 0.16 | 0.13 | 0.02 | 0.02 |
| picture, surface, upload | 0.08 | 0.07 | 0.02 | 0.02 |

Two separate problems:

- **About 1000 draw calls a picture at 009** (the pump's canvas: 27.2 million native draw calls for 28021 pictures at
  1x). They cost the GPU 0.5 ms at any resolution and the pump thread 1.46 ms, which is the limit at 1x.
- **About 1.7 ms of full-screen work a picture at 4x** on every map (020: 2d, present, copy, texture copy, picture),
  before anything of the scene is drawn.

### Phase 0: tools

Built first: each later phase is then measured before it is built, not guessed.

- **Knock-out switches.** One kind of work left out of the pictures between (the clears, the brightness pass, the
  scaling, the surface copies, native 280's draws), the rest timed. The picture is wrong; the time is what that work
  costs, and so the most its removal can give.
- **A frame ledger.** Every pass of one picture in order: its kind and state, its target, the draws, the pixels
  touched, the GPU time. The profiler has the time by state, without the order, the pixels or the draws. On
  2026-11-14 this was a temporary print in `draw_batch`.
- **GPU memory by image in `--stats`.** Every live image and buffer by size and kind, and what was made in all (the
  churn). On 2026-11-14 a temporary patch in Granite's `Image` and `Buffer`; it found the 96 MB image made for every
  picture.
- **A replay bench.** A recorded stretch of play through the renderer alone, pictures between and pump included,
  uncapped and repeatable: two builds compared in seconds, and the window's picture compared byte for byte (a window's
  screenshot is not the same between two runs of one build). To find out first: whether `kf4-gsreplay` drives the
  pictures between.
- **Test runs uncapped** (`--frame-rate-cap 0`), with pictures a second over the run in the stats, not the last half
  second.

### Phase 1: a thousand draws to a few dozen

A native batch's runs are a draw call each because each has its own record (the instance), and native 280's
alternate between two blends. Two changes, neither an indirect draw:

- The record's number goes with the triangle (a buffer beside `tris`, read by `gl_VertexIndex / 3`, or the kept walk's
  triangles written with it), and to the fragment stage flat, so consecutive runs of one state are one `vkCmdDraw`.
  Built when the walk is kept, 30 times a second, not for each picture.
- Both blends from one state: the factors from the shader with dual-source blending, as the kept walks' runs of #139
  have them (`PrimPush::factors`).

This is what #139's indirect draws were for, without `vkCmdDrawIndirect`, which hung the card on RADV. Not known: why
those runs drawn one `vkCmdDraw` each were slower (4.85 against 3.45 ms headless on 2026-11-13); the knock-out
switch for native 280 says what there is to gain before this is built.

Aim: native 280 from 0.5 ms to under 0.1 ms, the pump thread from 1.46 ms a picture to a few tenths.

### Phase 2: one pass from the surface to the window

Today a picture's last steps are separate full-screen passes: the buffer resolved and copied for the brightness pass
(texture copy), the brightness pass drawn over the buffer (2d), the scanout into the picture (picture), the scaling
with about 49 reads a pixel, the sharpening (present). They become one pass that reads the surface's samples and
writes the window's pixels: resolve, brightness, scale and sharpen together, with the scaling across then down if it
is still two.

- The brightness pass is found as the frame's last draw of the buffer over itself (texture the buffer drawn, colour
  the same at every corner: 136 of 128 at 009) and left out of the pictures between; its factor goes to the pass.
- The clears in the pictures between become real clears (`as_clear` refuses inside a picture between today).
- The game's own picture before the brightness step stays byte for byte; the brightness step and the window's picture
  do not (agreed). The replay bench compares the window's picture with the one before, with a tolerance to choose.

Aim at 4x: texture copy, picture and most of 2d and present, about 1 ms of 020's 1.7 ms.

### Phase 3: one set of surfaces

kf4-pc draws each frame, then the pump draws it again for every picture on copies of kf4-pc's surfaces: three sets of
surfaces (1.55 GB) and 0.26 ms a picture of copies. With the frame's draws made by the pump's canvas only, kf4-pc
keeps the walk and what the texture store needs, and the copies and one or two sets go. What kf4-pc must still draw
itself is the question to answer first: the surfaces the game reads back or reads as textures in a later frame.

### Where it could land

A guess, to be replaced by Phase 0's measurements: about 800 pictures a second at 4x at a light map like 020 (the
scene 0.8 ms and about 0.4 ms of the rest), about 500 at 009 (the scene's own pixel work, native 000 and 700, is 1.2
ms there at 4x). Past that the cost is the scene drawn at 4x with MSAA 2 and 16 bits a channel.

### The order

Phase 0, then Phase 1 (the same gain at every resolution, and the pump thread's limit with it), then Phase 2, then
Phase 3. The GPU hang stays open beside this; none of the phases needs indirect draws.

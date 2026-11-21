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
D=~/.cache/kf4hang/data    # a copy of ~/.local/share/holyland: settings.json and the pipeline cache
KF4_DATA_DIR=$D KF4_READ_SETTINGS=1 KF4_GSN_GPU_TIME=1 out/linux-clang/kf4 --disc <image> --no-audio --saves none \
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

### The hang watch (2026-11-14)

Not a fix: what a hang leaves behind. Until now the game stayed on the screen frozen and had to be killed, and
nothing on our side said what the GPU had been given.

- `pcr_hang_notes.h`: each submit to the GPU's queue leaves a note (the thread, and for a canvas what it recorded
  since its last note: batches, native batches, passes, merged and indirect runs, surface copies, clears), the last 64
  kept. Each call that waits for the GPU says so while it waits: kf4-pc's `next_frame_context` (where the frozen runs
  sat), the vblank's, the read-backs' fences.
- `pcr_hang_watch.h`: a thread ("kf4-pc-watch") looks once a second. A wait that has lasted 15 of its looks is a hang:
  the notes go to `<data folder>/gpu_hang_<date>_<time>.txt` and stderr, and the game ends (exit code 70).
  `KF4_PC_HANG_WATCH=0`: no watch. The folder comes in by `pcr_hang_reports_in` (pcr.h), before `pcr_open`.
- After a hang: send the report, and the kernel's lines (`journalctl -k | grep -A3 'ring gfx'`).

Checks: unit tests `pcr_hang_notes` and `pcr_hang_watch` (a wait that never ends ends a child process with the report
written; waits that end in time do not), under ASan with UBSan and under TSan too; 20 s at 009 in a real window with
the owner's settings: no report, 3.32 ms a picture (3.38 before: no cost). Not run: a real hang (none came), the game
whole under the sanitizers, the gcc and Windows builds.

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

## What a picture's layers are at 009, and what a modern renderer's tricks give (2026-11-14, afternoon)

Asked: is the cost the PS2's way of drawing, which a modern renderer would not pay? Measured, not built (the
experiments' code is not kept). Real window, the owner's settings, 009:0, 15 s each; 3.34 ms a picture as it is.

**The layers.** One frame captured (`--gs-capture-at`), every fragment at 176 pixels of a grid traced
(`kf4-gsreplay --renderer pc --scale 1 --pixel X,Y`). Fragments a pixel, on average:

| Kind | Drawn | Failed the Z test |
|---|---|---|
| full-screen sprites, not blended (the clear, the same clear again, the brightness pass) | 3.0 | |
| full-screen sprites, blended (the destination alpha's reset, the HUD) | 1.2 | |
| the fire (native 700; all on the right of the picture) | 3.2 | 0.6 |
| the map (native 000) | 1.5 | 0.3 |
| native 280 | 0.02 | |

About 10 fragments a pixel, and **only one in ten is hidden**. A wall's pixel is one map fragment and four full-screen
passes. The destination alpha's reset is for the shadows after it (352 strips of program 0x5A0 with DATE and FBA: each
pixel darkened once).

**What was tried.**

| Experiment | ms a picture | Says |
|---|---|---|
| every native batch's Z drawn first, then the scene (exact Z on, and off so that the early test rejects) | 3.41 to 3.48, 3.35 to 3.32 | nothing hidden to leave out: a depth pre-pass or a sort front to back gives nothing here |
| DATE's stencil set without reading the target's alpha (as if it were known from the reset) | 3.34 to 3.28 | 0.06 |
| the untextured GIF batches left out (`KF4_PC_KNOCK=fills`) | 3.00 | the clears, the alpha's reset, the fade: 0.33 |
| and the brightness pass's draw (`fills,surface`) | 2.89 | 0.45 together; with its texture copy (0.16) about 0.6 |

So the PS2's way costs, at most, the full-screen passes: **about 0.6 to 0.75 ms of 3.34** if every one of them went
(Phase 2 whole, and the alpha's reset kept in the stencil alone). The rest is the scene's own visible layers at 13.8
million samples of 8 bytes and their depth: about 1 to 2 GB moved a picture, at about 300 pictures a second half or
more of what the card's memory moves (640 GB a second). Fewer samples or fewer bytes a sample is all that is left
(round 3's table).

**The machine went down** at 14:56 as the third run in a row started (`KF4_PC_KNOCK=n700`, in the opening): no ring
timeout in the kernel's log, the log just stops. Twelve real-window runs before it in an hour and a half without a
hang.

## What a layer costs on this card: `kf4-layerbench` (2026-11-14, later)

`tools/layer_bench` (built as `kf4-layerbench` where `glslangValidator` is): plain full-screen layers on a target of the
renderer's size and formats, no game in it, one state changed at a time, timed by the clock over 200 pictures of 8
layers. RX 9070 XT, RADV 26.2.3, 3840x1792. ms a layer:

| A layer that is | 8 bit, 1 sample | 8 bit, 2 | 8 bit, 4 | 16 bit, 1 | 16 bit, 2 (the owner's) | 16 bit, 4 |
|---|---|---|---|---|---|---|
| two triangles, textured, depth, blended | 0.039 | 0.052 | | 0.137 | 0.166 | |
| 45000 triangles (about 300 pixels each: the map's are about 500), textured, depth | 0.043 | 0.128 | | 0.087 | 0.428 | |
| 45000 triangles, textured, depth, blended | 0.055 | 0.210 | 0.565 | 0.191 | 0.565 | 0.955 |
| one colour, no depth, no blend | 0.012 | 0.023 | | 0.021 | 0.042 | |
| hidden (fails the early depth test) | 0.011 | 0.012 | | 0.011 | 0.020 | |

What it says:

- **A layer of many triangles under MSAA costs three to four times the same layer at one sample**, in either colour
  format, where a layer of two triangles costs a quarter more. The cost is the triangles' edges under MSAA, not the
  bytes: 8 bits with 2 samples (0.210) is dearer than 16 bits with 1 (0.191) at half the memory.
- **The owner's pair, 16 bits and 2 samples, is ten times 8 bits and 1 sample** for a map-like layer (0.565 against
  0.055); either setting alone is about 0.2.
- 16-bit floats cost what 16-bit UNORM does with MSAA; 10 bits a channel what 8 does (but has 2 bits of alpha).
- The driver's switches: `RADV_PERFTEST=dccmsaa` and `RADV_DEBUG=nohiz` change nothing (`dccmsaa` is for GFX8-9
  only). `RADV_DEBUG=nodcc` (a layer of 45000 triangles, textured, depth): 8 bits with 2 samples 0.129 to 0.448, 16
  bits with 2 samples 0.429 to 0.653. The colour compression works under MSAA for both; it saves more at 8 bits.
- **This card does compress MSAA colour**: Mesa 26.2.3's `ac_surface.c` (`gfx12_compute_surface`) turns DCC on for
  every multisampled colour image ("Always enable compression for Z/S and MSAA color by default"). What it no longer
  has is FMASK, the older cards' own MSAA scheme (`ac_gpu_info.c`: `has_fmask = gfx_level <= GFX10_3`, RDNA 2 and
  before). Why many small triangles make MSAA three to four times dearer: the next section.
- An image twice as tall as what is drawn (the renderer's frame buffers' surface) costs nothing more. A render pass a
  layer, a shader that may discard, one that writes its own depth: nothing to speak of.
- **The renderer is not far off the card**: the map's 1.5 layers at about 0.56 ms is 0.85 ms, its knock-out 1.2 with
  the fragments that fail. There is no hidden factor of several in how the renderer draws; there is one in what MSAA
  costs here.

So the lever is anti-aliasing that is not MSAA. Supersampling is cheaper for the same count of samples: twice the
pixels at 1 sample is about 0.11 ms (8 bit) or 0.38 (16 bit) a map-like layer, against MSAA 2's 0.21 and 0.565. Not
yet measured in the game: 4x with MSAA 1 and colour exact together, and a higher scale without MSAA.

## Why MSAA costs so much here: pixels written in part (2026-11-14, later still)

New cases in `kf4-layerbench` (`edges:`, `split:`, `part:`) take the map-like layer apart. RX 9070 XT, RADV 26.2.3,
3840x1792, ms a layer, textured with depth unless said:

| | 8 bit, 1 sample | 8 bit, 2 | 16 bit, 1 | 16 bit, 2 (the owner's) |
|---|---|---|---|---|
| 2 triangles | 0.025 | 0.036 | 0.043 | 0.087 |
| 1250 triangles (quads 128x60 pixels) | 0.027 | 0.042 | 0.047 | 0.175 |
| 5000 | 0.031 | 0.056 | 0.057 | 0.237 |
| 45000 | 0.044 | 0.132 | 0.090 | 0.426 |
| 180000 | 0.057 | 0.179 | 0.105 | 0.465 |
| 45000, colour alone (no depth) | 0.035 | 0.054 | 0.058 | 0.262 |
| 45000, depth alone (colour masked off) | 0.035 | 0.059 | 0.037 | 0.090 |
| 2 triangles, the shader's sample mask all samples | 0.072 | 0.072 | 0.072 | 0.147 |
| 2 triangles, the sample mask only sample 0 | 0.070 | 0.116 | 0.072 | 0.230 |
| 45000, `RADV_DEBUG=nodcc` | | 0.444 | | 0.650 |
| 45000, `RADV_DEBUG=nobinning` | | 0.129 | | 0.434 |

What it says:

- **The cost is pixels whose samples are written at different times**, not the triangles. A triangle's edge leaves the
  pixels it crosses part-covered, and the triangle beside it writes their other samples later. Two triangles with a
  sample mask of one sample (every pixel part-written, no edges at all) cost what 5000 to 45000 triangles do. At one
  sample no pixel is ever written in part, and the triangles cost little.
- **It levels off**: 180000 triangles cost a tenth more than 45000. Once every block of memory holds a part-written
  pixel, more edges add nothing. That is the shape of writes that no longer fill whole blocks, which the memory side
  must read, merge and write back (and recompress). It is the same with the colour compression off (2 triangles 0.245,
  45000 0.650 at the owner's), so it is not only DCC giving up; DCC still saves a third.
- **It is the colour, and the bytes a pixel holds**: colour alone with edges is 0.054 ms at 8 bytes a pixel (8 bit, 2
  samples; or 16 bit, 1 sample: 0.058) and 0.19 to 0.26 at 16 (16 bit and 2 samples, or 8 bit and 4). The depth adds
  0.06 to 0.09 at 2 samples. The owner's pair is past the step between 8 and 16 bytes a pixel.
- Edges in new places each layer (as the game's are) cost the same as edges in the same places. Binning off changes
  nothing. The renderer's image twice as tall: nothing.
- Writing `gl_SampleMask` itself costs (0.07 even at one sample), so the `part:` rows compare with each other, not with
  the `edges:` rows.
- Why this card has the step: the hardware's own documents would say. FMASK is still a fair guess (with it, a
  part-covered pixel's samples were written as one colour and a mask; without it each sample is its own write), but
  nothing here proves it.

**The game does nothing wrong here.** It draws the PS2's triangles as the PS2 did, and its MSAA cost at 009 (0.55 ms,
round 3) is what this table says 1.5 map layers cost. Its own MSAA extras are small: cut-out AA 0.17 ms, resolves and
expands within the knock-outs' noise. What it could change:

- At 4x on a window smaller than 3840x1792, the scale is already supersampling: MSAA 2 on top buys little for its
  0.55 ms. MSAA's price is lowest at 1x or 2x, where it is worth most.
- The step is at 8 bytes a pixel: colour high with MSAA 2 was the one combination past it. **Colour high is removed**
  (2026-11-14): it showed no difference to see, and the colour surfaces are now always 8 bits a channel.
- The depth with stencil (`D32_SFLOAT_S8_UINT`) is a fifth of it; `D32_SFLOAT` where the stencil is unused would save
  part of that (not measured).

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

### Unlocked's cap, and a GPU hang (2026-11-14, later)

The owner asked for unlocked's cap to be removed altogether. It is not removed yet: with today's pacing, asking for
more pictures than the renderer makes shows fewer. The slider at 0 (`"frame_rate_cap": 0`) is no cap already.

Real window, the owner's settings, 4x at 009, 30 s of play; "asked" is the pictures a second the schedule asks for:

| Asked | Pictures handed to the window | Pictures between shown | Between new pictures |
|---|---|---|---|
| 500 (the owner's cap), two runs | 12008, 12095 | 8394, 8484 | 7.4 ms (sd 14) |
| 959 (no cap: the schedule's most), two runs | 9453, 10260 | 5989, 6541 | 9.4 to 10.2 ms (sd 18) |
| a quarter more than the window showed in the last half second, one run | 12912 | 12054 | 5.35 ms (sd 5.9) |

- **No cap costs about a fifth of the pictures shown at 4x**: the pump leaves most of what is asked for out and makes
  more too late.
- **The schedule has a most of its own, 16 a vblank (959 a second)**, which 1x at 020 reaches (`UNLOCKED_MOST_A_VBLANK`,
  and `INTERP_ALL` in gs.c, which must go with it). Raised to 64 (3836 a second), with the cut's steps an eighth of
  the count: at 1x at 020 more pictures were drawn (56825 for 41429) and fewer shown between vblanks (28992 for
  46450); at 4x at 009 a quarter fewer reached the window (7558 for 10035). Taken out.
- **An asked rate that follows the window** (a quarter more than `port_video_pictures_per_second()`, between 120 and
  the schedule's most) showed 43% more pictures between than the cap of 500 and far more evenly, in its one run. This
  is the way to no cap: nothing to set, and no pictures made for nothing. Not built: one run, and the next run hung
  the card.
- **The GPU hang again**: 01:30:43, `ring gfx_0.0.0 timeout`, thread kf4-pc-pump, then a reset of the card. A real
  window, 4x at 009, 68 s into the run (about 30 s of play), the asked rate one and a half times what the window
  showed, indirect draws off, the game holding 3.9 GB. So it does not need the memory high, nor minutes of play.

To the plan: **Phase 1b, unlocked without a schedule's rate.** The asked rate follows what is shown (or the pictures
are made as fast as they can be, each for the time it reaches the screen), the cap's setting, slider and option go,
and the schedule's 16 a vblank with them. Measured with the replay bench and in a real window against the table
above.

## What changed on 2026-11-14 (morning): Phase 1, merged draws

Branch `perf/speed-plan`. A kept walk's native batches are drawn as **merged runs** in the pictures between: one
`vkCmdDraw` for every stretch of batches with the same pass, program, depth state and write mask, however many batches
and whatever their blends and textures. Not an indirect draw (those stay off on RADV).

How: each triangle of a merged draw has an entry in a buffer of the picture's (`MergedTriangle`, `u_merged`): its
triangle in the program's triangles, its instance and its batch's record. The native vertex shaders read their
triangle through it (`pcr_triangle`, pcr_common.h). The two blends of native 280 are one state: the factors come from
the shader (the dual-source blend #139 built). `KF4_PC_MERGED=0` turns them off, `=plain` leaves the dual-source blend
out, `=1` uses them where the indirect runs are on too (NVIDIA: not tried there; the indirect runs stay as they were).

Real window, the owner's settings, 009, `--frame-rate-cap 0`, 30 s of play, off and on from one build:

| | off | merged |
|---|---|---|
| 1x: GPU ms a picture | 1.174 | 0.825 |
| 1x: native 280 | 0.478 | 0.135 |
| 1x: pictures in the run | 38669 | 48936 |
| 1x: the pump, ms a picture at the end | 1.47 | 0.74 |
| 1x: left out as too late | 13903 | 3300 |
| 1x: native draw calls a picture | 961 | 284 |
| 4x: GPU ms a picture | 3.703 | 3.691 |

- **At 1x the game is no longer held by the pump**: it drew nearly every picture the schedule asked for (51475 asked,
  the schedule's own most of 959 a second: Phase 1b).
- **At 4x nothing changed**: the GPU's time there is pixel work. The kinds' shares moved (native 280 0.58 to 0.22, 2d
  0.90 to 1.27) but their sum did not: the timestamps sit between draws, and the pixel work of a draw ends later. So the
  "native 280: 0.6 ms that no setting lowers" above was the draw calls only at 1x; at 4x it is pixels under another
  name. Phases 2 and 3 are what 4x needs.
- About 150 merged runs and 284 draw calls a picture are left of 961. What ends a run now: a change of program (000,
  280, 700), of depth state or write mask, a GIF batch, a batch not from the walk's table.

Three things the pictures needed, each found by comparing byte for byte:

1. **`nonuniformEXT` on the table's texture and sampler index** in the runs' fragment shaders: a merged draw reads
   several textures, and pixels of two batches in one quad's wave read one or the other's.
2. **No `textureSize` of a table's texture**: on RADV (Mesa 26.2.3) it gave another batch's texture's size here and
   there, `nonuniformEXT` or not (about 150 pixels a picture, changing run to run). The size now comes with the
   record (`PrimPush::src_offset`, pcr_prim.h's `sampled_size`). A driver bug, it seems: not reported yet.
3. **The dual-source blend's one minus by the GPU's maker**: NVIDIA's blend cuts a factor to the target's precision
   and RADV's on AMD does not (`F_DUAL_CUT`); with NVIDIA's cut, 9:2 and 0:0 came out a value darker at about 850
   pixels a picture. Other makers get merged runs without the dual-source blend until their pictures are compared.

Checks: pictures byte for byte against `KF4_PC_MERGED=0` (52 pictures each, headless at 240 with the pictures between,
the owner's settings: 000:0, 009:0, 009:2, 020:0, 105:0; and 009:0 at 1x without MSAA, colour exact); all 20 unit
tests; the game under ASan and UBSan in a real window; no GPU hang in any run. Not run: TSan, the gcc and Windows
builds, NVIDIA, a map tour against `--renderer gs`.

Next, from this: the pump still walks every batch for every picture (`draw_batch`, about 900 a picture) to make the
same records and entries each time; they could be made once a walk (30 a second). And the GIF batches end runs.

## What changed on 2026-11-14 (round 2): what 4x costs, and the scaling in two passes

### Knock-outs (`KF4_PC_KNOCK`)

`KF4_PC_KNOCK=kind[,kind]` leaves one kind of work out of the pictures between (`knocked_out`, pcr_canvas.cpp:
clears, fills, surface, 2d, n000, n280, n700, blended). The picture is wrong; the GPU time a picture less is what that
work costs. The game's own frames (one picture in four here) are still drawn whole, so the numbers are low by about
a quarter. Real window, the owner's settings, 009, uncapped, 15 s of play; with nothing knocked out 3.60 ms:

| Left out | ms a picture | Less |
|---|---|---|
| native 000 (the map) | 2.40 | 1.20 |
| every GIF batch (2d) | 2.81 | 0.79 |
| native 700 | 3.10 | 0.50 |
| the untextured GIF batches (fills: the clears and the full-screen blends) | 3.15 | 0.45 |
| native 280 | 3.44 | 0.16 |
| the batches textured from a surface (the brightness pass) | 3.51 | 0.09 |
| the clears | 3.55 | 0.05 |
| all four kinds | 0.81 | 2.79 |

And one setting changed (15 s each):

| Setting | ms a picture |
|---|---|
| the owner's | 3.60 |
| anisotropic 1, not 16 | 3.58 |
| exact Z off (`KF4_PC_EXACT_Z=0`) | 3.63 |
| cut-out AA off | 3.43 |
| MSAA 1 | 3.05 |
| colour exact | 3.02 |
| scaling smooth, no sharpening (the presenter's own) | 3.10 |
| `RADV_DEBUG=nodcc` (the driver's colour compression off) | 6.12 |
| `RADV_DEBUG=nohiz` | 3.59 |

What this says:

- **4x is bandwidth.** No shader feature costs anything to speak of (anisotropy, exact Z); the samples and the bytes a
  sample do (MSAA 2 and 16 bits a channel: 0.55 ms each). A full-screen layer at 3840x1792, two samples, 8 bytes a
  sample is 110 MB written, and read too when blended. The map (native 000) is 1.2 ms of it.
- **The clears are worth 0.05 ms, not the 0.32 the profiler's row said.** They are cleared now in the pictures between
  too (below): `vkCmdClearAttachments` inside a pass is a rectangle drawn by the driver, the same bytes. Only a pass's
  load op clears for nothing, and that needs a surface that is one buffer, not both (Phase 3).
- **The profiler's kinds cannot be trusted at 4x** for what a kind costs: the timestamps sit between draws and the pixel
  work ends later. The knock-outs can.
- HiZ does nothing here (with the stencil never used, `KF4_PC_STENCIL=0`, too). The driver's colour compression does a
  great deal: whatever is done to the surfaces must keep it.
- With everything of the scene left out 0.8 ms stays: present 0.49, the texture copy 0.11, the picture 0.06.

### Built

- **The scaling in two passes** (`pcr_present_scale_1d.frag`, `PresentScaler`): across into an image as wide as the
  window and as tall as the picture (linear light, RGBA16F), then down. Present 0.477 to 0.206 ms a picture, the
  picture **3.60 to 3.30 ms**. The window's picture is not the one pass's bit for bit: each pass clamps the cubic's
  overshoot along its own direction, the one pass clamped once over both. Against the one pass (`--dump-present
  2560x1440`, 52 pictures each at 009 and 020): 95% of the values the same, 3.5% one off, 1% three or more off (hard
  diagonal edges, 41 at most), nothing to see side by side. `KF4_PC_SCALE_PASSES=1`: the one pass. It holds 37 MB more
  for each thread that makes pictures.
- **Clears in the pictures between** (`Batch::still`, `kick_stays` in pcr_frame_walk.h with a unit test): a batch of
  the pictures' walk whose kicks' moves are all "as it is" is where the frame put it, so `as_clear` may clear it.
  About 2.6 a picture at 009; worth 0.05 ms (above).

Checks: the game's own picture byte for byte against round 1's (52 pictures each at 009:0, 009:2, 020:0); all 20 unit
tests; ASan and UBSan in a real window (the known reports); no GPU hang. Not run: TSan, gcc and Windows builds, NVIDIA.

## What changed on 2026-11-14 (round 3): 4x is the card's fill rate

More knock-outs and experiments at the owner's settings (009, real window, 15 s each), after round 2's 3.30 ms:

| Experiment | ms a picture | Says |
|---|---|---|
| the surface copies for the pump left out (`KF4_PC_KNOCK=copies`) | 3.24 | the copies cost 0.07 ms, not the profiler's 0.27 |
| every native draw with a trivial fragment shader (no texture, no fog, no tests) | 2.94 | all the shading is 0.33 ms of the natives' 1.9 |
| blends that are their source drawn without the blend (kept, below) | 3.25 | reading the destination is little of it |
| a depth-only pass before the map | no change | the map is mostly drawn blended: nothing qualifies |
| HiZ off, exact Z off, anisotropy off (round 2) | no change | |

So at 4x with two samples and 16 bits a channel the time is **rasterising and writing that many samples**: 13.8 million
samples a layer, about 26 bytes each with depth, about three layers deep. No shader or state change gets it back;
fewer samples do:

| Setting (009, the same build) | ms a picture | The game's GPU memory |
|---|---|---|
| the owner's: 4x, MSAA 2, colour high | 3.24 | 3.8 GB |
| 4x, MSAA 2, colour exact | 2.58 | 3.1 GB |
| 4x, MSAA 1, colour high | 2.65 | 2.7 GB |
| 3x, MSAA 2, colour high | 1.74 | 2.8 GB |
| 2x, MSAA 2, colour high | 1.05 | 2.4 GB |

4x draws 3840x1792 for a 2560x1440 window; 3x draws 2880x1344, about the window's own size, in a little over half the
time. That is the owner's choice to make, not a default changed here.

Built:

- **Blends that are their source are not blended** (`blend_is_source`, `KF4_PC_UNBLEND=0` to compare): (Cs - Cd) * C +
  Cd with FIX 0x80, or As 0x80 at every pixel: the vertices' alpha all 0x80 (a GIF batch's own; program 0x000's from
  its strips' colours, `RdBatch::alpha_lo/hi`, as `NativeBatch::alpha_is_one`) and the texture's too where TCC takes
  it. Half of 009's blended map triangles have alpha 0x80 throughout; about a third of the blended batches qualify
  (the others take a texture's alpha that is not 0x80 everywhere). 0.05 ms. Pictures byte for byte (below).
- `KF4_PC_KNOCK=copies`.

Checks: pictures byte for byte against round 1's at 000:0, 009:0, 009:2, 020:0, 105:0 (52 each); the unit tests (the
`sampler` test failed once under load and passed again: it times threads); ASan and UBSan in a real window (the known
reports).

### Two more GPU hangs (06:58:44 and 09:26:16)

Both `ring gfx_0.0.0 timeout`, thread **kf4-pc**, in the **opening** (24 s and about 15 s into the run, before the
teleport at vblank 1900), both runs with `--frame-rate-cap 0`: one the committed build with `KF4_PC_UNBLEND=0`, one at
3x with MSAA 4 (pipelines compiling). 2 hangs in about 60 real-window runs this morning. So: not the pump, not play,
not the memory. The opening uncapped runs at the schedule's most (959 pictures a second) on the GPU's simplest
pictures. Test runs here no longer pass `--frame-rate-cap 0` (a picture's GPU time does not need it).

What would find it, and needs the owner (root, or a package):

- **The kernel's dump of the hung job**: for five minutes after a hang, `sudo cat /sys/class/devcoredump/devcd*/data
  > ~/hang.txt` (root only). It names the ring's last commands.
- **The validation layer**: `sudo dnf install vulkan-validation-layers`. The copies on this machine (Steam's, 1.3.224;
  Flatpak's) are too old: the game crashes under them at start.

## What changed on 2026-11-14 (round 4): unlocked follows the window (Phase 1b)

Unlocked asked for a picture every 1 / the cap of a second (500 for the owner) whatever the renderer made. At 4x it
makes about 300, so most of the schedule's pictures were left out as too late and those made came in bursts. Now the
asked rate is **a quarter more than the window showed in the last half second** (`unlocked_asked_hz`,
frame_interp.c), never under 120 nor over the cap or the schedule's most. `KF4_UNLOCKED_FOLLOW=0`: as before.

Real window, the owner's settings (cap 500), 20 s of play, fixed and following from one build:

| | pictures between shown | between new pictures | shown late |
|---|---|---|---|
| 4x, 009: fixed | 7489 | 7.08 ms (sd 13.3) | 806 |
| 4x, 009: following (three runs) | 10405 to 10595 | 5.2 ms (sd 6.1 to 7.3) | 113 to 129 |
| 4x, 020: fixed | 15775 | 3.52 ms (sd 9.1) | 183 |
| 4x, 020: following | 16055 to 16157 | 3.47 ms (sd 5.4) | 64 to 67 |
| 4x, 105: fixed | 15958 | 3.50 ms (sd 9.1) | 194 |
| 4x, 105: following | 16209 | 3.45 ms (sd 5.5) | 77 |
| 3x, 009: fixed | 20545 | 2.73 ms (sd 4.1 to 4.9) | 39 to 42 |
| 3x, 009: following | 19440 | 2.90 ms (sd 3.5) | 37 |
| 1x, 009: fixed | 21762 | 2.60 ms (sd 3.7) | 37 |
| 1x, 009: following | 20712 | 2.73 ms (sd 3.2) | 37 |

- **Where the GPU is the limit (the owner's 4x) 40% more pictures between reach the window at 009, half as unevenly,
  and a seventh as many late.** At the lighter maps a little more, and much more evenly.
- **Where the cap is reached (1x, 3x) 5% fewer in a run**: after the menus' 60 and after a load the rate climbs a
  quarter each half second, about three seconds from 120 to 500. In play it sits at the cap as before.
- Tried and left out: the top at once (or twice as many) where the window showed all that was asked for, and one
  answer for each of the window's counts. Each made play worse than the plain rule (4x at 009: 9300 pictures between,
  sd 9; the rate overshoots after any dip and the bursts are back).
- **The cap's slider stays** (the owner asked for it gone): it is no longer needed to make unlocked smooth, and 0 (no
  cap) is now safe, but a VRR screen still wants a rate held under its refresh. The owner's to take out.

Also: `vu1_280_maths.h`'s `colour_u` walked a strip's colour back k steps, **k read from the mesh with no bound**: a
wrong number there is a loop of up to two thousand million steps a vertex, which is a GPU ring timeout. Bounded at
256 (a VU1 batch's most vertices); pictures byte for byte at 009:0 and 009:2. Not shown to be the hang's cause: the
only loop in the shaders whose length comes from data.

A third hang, 10:10:10: thread kf4-pc, about 42 s into a capped 4x run at 009 (10 s of play), before the bound above
was in. A 20-run test of the opening alone, uncapped, did not hang: the opening is not a trigger by itself.

Checks: the 20 unit tests; pictures (the shader's bound) byte for byte at two spots. Not run on this round: the
sanitizers (a C function of arithmetic and one atomic), TSan, gcc and Windows builds.

## What changed on 2026-11-14 (round 5): dead writes left out of the pictures between

What the game's full-screen passes are at 009 (a dump of the pictures' walk; 640x448 each, in order): the clear,
**the same clear again** in the other context, the scene, the destination alpha set to 0 (ten sprites, a blend by FIX
0 that leaves the colour), later a Z-only clear, and when a fade is on a black blend over everything, then the
brightness pass, and **Z cleared once more** after the last draw that tests it.

`pcr_dead_writes.h` (header only, unit test `pcr_dead_writes`): a batch whose every write is written over at every
pixel, or never looked at, before anything reads it is not drawn by the pump. A pictures' walk is summarised a step
at a time (`walk_step`: what it may read, write, and writes at every pixel, by its registers; sprites that keep their
place, `kick_keeps_place`, and cover their scissor), the walk taken as drawn round and round on the pump's surfaces,
the picture taken at its end reading the colour. Every doubt is a read. Only the pump leaves them out (kf4-pc's
surfaces are the game's own). `KF4_PC_DEAD=0`: all drawn.

At 009: the first clear and the last Z clear, 1.7 batches a picture: **3.46 to 3.38 ms a picture** (two runs each,
15 s). The alpha's pass is not dead in play (draws that test the destination alpha follow it).

2026-11-15: at 063 the first clear was kept too: `mark_dead_writes` took every frame mark it did not know as reading
everything, and frame_interp.c's camera and part marks (0x0D-0x16), which the canvas does nothing with, sit between
the two clears and between the alpha's pass and the fade. Now only 0xF2 (the frame shown kept) reads. Batches left
out at 063 twice as many (12.6k to 25.5k a 20 s run), 2.584 to 2.567 ms a picture (2-3 runs each: about the noise).
The alpha's pass at 063 (0.08 ms when left out) is still drawn: the fade after it, which writes the alpha everywhere,
has its corners from a native draw's kicks (the trail's), so it is not known to keep its place.
The pass is cheaper drawn instead: its blend is Cs * FIX + Cd with FIX 0, which leaves the colour, so `choose_blend`
now draws it (and D - Cs * FIX with FIX 0) without the blend, the alpha alone written, as it did A = B with D Cd:
no read of every pixel's colour. 063: 2.611 to 2.557 ms a picture (two runs each).

Checks: pictures byte for byte at 000:0, 009:0, 009:2, 020:0, 105:0 (52 each); the 21 unit tests, the two new ones
under ASan and UBSan too; the game under ASan and UBSan in a real window (the known reports); no hang.

Looked at and left: **GPU memory.** What is left is the three sets of surfaces (1.5 GB), and getting one back means
the pump keeping surfaces of its own, which needs to know that a walk reads nothing it has not drawn (the brightness
pass's copy takes 512 rows where the clear covers 448: a rule would refuse it at once), for 0.5 GB. The flash
recorder's ring is 0.55 GB while it is on (the owner's setting; off, it is nothing), and 3x in place of 4x is 1.0 GB.

## A Vega 7 at 1080p (2026-11-18, branch perf/low-end-1080p)

Everything above was measured on an RX 9070 XT. This is the low end: a laptop with a Ryzen 5 5625U (6 cores, Vega 7
integrated graphics, RADV "RENOIR", 512 MB carved out of 16 GB of DDR4 shared with the CPU), a 1920x1200 60 Hz panel.

**Settings** (a copy of the data folder, `KF4_READ_SETTINGS=1`): a 1920x1080 window, widescreen with native widescreen,
resolution auto (2x: the canvas 1920x896 a buffer), SMAA T2x, sharpen 0.5, scaling best, texture quality Maximum (LOD
full, anisotropic 16), draw distance 4x, sky modern, reflection cut, frame rate unlocked with no cap, vsync off. A real
window on the screen, `tests/newgame.pad` and `--teleport 1900:MAP:0 --invincible`, 25 s measured, `KF4_GSN_GPU_TIME=1`.

**On battery the numbers move.** The first half of the work was on battery (balanced-battery profile): the GPU's
clock fell to 400 MHz now and then and the memory's between 800 and 1333 MHz, about 5% between runs, and once 20%
when the battery ran down. The final numbers are on mains power (performance profile), the baseline (main at 792b2788)
and the branch alternated, two runs each: the kinds of work not changed agree within 1% between the builds.

### Where the time went (main, 009)

The **GPU is the limit**: the game's thread spends 74% of its time waiting for the DMA, which waits for the renderer.
17.0 to 17.2 ms of GPU a picture, about 58 a second at best, and the game itself fell to 27-29 frames a second on battery
(each game frame needs its own picture and one between: two pictures in 33 ms).

| Kind | ms | What it is |
|---|---|---|
| 2d | 3.7 | the full-screen fills (1.8: four a game frame, 0.9 ms each), the brightness pass (0.45), shadows' DATE (0.5) |
| picture | 3.7-3.8 | the scanout and SMAA T2x: depth copy, motion, the picture's copy, edges, weights, blend, resolve |
| native 000 | 3.2 | the map |
| present | 2.6 | scaling to the window (best) and sharpening |
| native 700, 280 | 1.3, 1.1 | |
| texture copy, planes, copy | 0.6, 0.6, 0.3 | |

What each setting costs (009, main, on battery; one setting changed): anti-aliasing off 15.3 ms (T2x 3.4 ms, SMAA 2.4,
FXAA 1.2); scaling smooth without sharpening 13.2 with AA off (best and sharpening 2.1 ms more); texture quality High in
place of Maximum, and draw distance 1x in place of 4x, no measurable difference at 009. **This GPU is short of memory
bandwidth and of arithmetic both**: a full-screen pass at 1920x896 costs 0.3 to 0.6 ms here where it cost 0.02 to 0.05
on the 9070 XT, and the scaler turned out to be bound by its arithmetic (below).

### What changed

| | 009 main | 009 branch | 020 main | 020 branch |
|---|---|---|---|---|
| GPU ms a picture (two runs each) | 17.18, 17.01 | 15.21, 15.17 | 14.31, 14.31 | 12.47, 12.45 |
| picture (SMAA T2x) | 3.80, 3.67 | 3.01, 3.00 | 3.69, 3.61 | 3.00, 2.96 |
| present (scaling, sharpening) | 2.62, 2.61 | 1.35, 1.35 | 2.59, 2.61 | 1.36, 1.33 |
| most GPU memory held (VRAM + GTT) | 4.38 GB | 3.09 GB | 4.37 GB | 3.08 GB |

About 1.9 ms a picture (11-13%), and 1.3 GB.

1. **Scaling to the window: one pass where one direction is already the window's** (`PresentScaler`, pcr.cpp). At 2x
   with native widescreen the picture is 1920 wide, as the window is: the across pass was the identity (the cubic's
   weights at whole pixels are 1 and 0, and its clamp keeps the pixel) and still wrote 1920x896 at 8 bytes a pixel. Now
   the other direction's pass decodes and encodes alone. About 1.0 ms.
2. **The picture's sRGB decoded by the sampler**, not by `pow()` in the shader: the picture is made with an sRGB view
   (`IMAGE_MISC_MUTABLE_SRGB_BIT`) and the scaling reads it through that. Three `pow()` a pixel read and five reads a
   pixel made; about 0.6 ms. `KF4_PC_SCALE_SRGB=0`: the shader decodes, as before.
3. **SMAA T2x reads the picture without copying it**: T2x blends into its history image, not the picture, so the copy
   SMAA 1x needs (its blend writes the picture it reads) is left out. About 0.3 ms. The weights pass's clear is left out
   too (it writes every pixel).
4. **The flash recorder's ring on an integrated GPU: 128 MB at most** (`flash_ring_most_bytes`; it was 512, allocated
   as 776 MB, more than the 512 MB carve-out by itself), and **not made again for the rate while nothing is shown, nor
   more than every 10 s** (`flash_ring_keeps_rate`): unlocked's rate is its ceiling (959) while nothing is shown,
   through a load or a teleport, and the ring was freed and made again at each, with a wait for the GPU to be idle,
   three times in 20 s.
5. **The presenter leaves out its clear of the window** when the picture covers it all (16:9 in a 16:9 window): a whole
   window written a picture less (not in the GPU time above, which is the renderer's).
6. **The game's clear of the screen is cleared, not drawn** (`as_clear`): it runs from Y -0.5 to 223.5, its field's half
   pixel, which at 2x and 4x is a whole pixel of the target; the rule asked for whole GS pixels. About 700 more clears in
   a 20 s run. It costs the same here (0.9 ms either way: the fill's memory, not the shader), and is exact.
7. **Fixed: the jitter moved sprites.** `DrawState::prim` has no primitive type (`prim & 0x7F8`), so SMAA T2x's test for
   sprites, `bits(s.prim, 0, 3) == 6`, never held, and the screen's untextured fills and the HUD's untextured sprites
   that test Z were jittered with the 3D. Now `b.sprites`. No difference in the pictures compared (the fills cover the
   whole screen).

Tried and taken out: the last scaling pass and the sharpening in one compute pass (shared memory, a 16x16 tile and its
border): 3.07 ms against 1.78, the scaling's arithmetic done twice over by the border's threads.

### Checks

- Pictures byte for byte against main (headless `--dump-frames`, 2x native widescreen): AA off at 009 (three vblanks,
  2x and 4x) and 020; SMAA 1x and SMAA T2x at 009: identical.
- The window's picture (`--dump-present 1920x1080`): within one level of main's at every pixel without sharpening (6%
  of pixels differ by one, from the image between's half floats no longer there and the sampler's exact sRGB decode);
  sharpening 0.5 takes that to at most 4 at 12% of pixels.
- The 37 unit tests; `flash_ring_test` has the heap's limit and the rate's keeping.
- Real-window runs at 009 and 020 on mains power, no GPU hang, no validation errors in the log.
- The game under ASan and UBSan in a real window at these settings, 20 s at 009: the known reports only
  (`SoundDriver_Remote`, the decomp's casts, Granite's `render_pass.cpp:52`).
- Not run: TSan, the gcc and Windows builds, a map tour against `--renderer gs`, the 9070 XT (the owner's card).

### Found and not done

- **The four full-screen fills a game frame, 0.9 ms each** (1.8 ms a picture; two of them done on 2026-11-19, below): the game's colour and Z clear, a second
  clear, a Z-only clear and the Z clear after the last draw that tests it. The pump leaves the dead ones out of the
  pictures between (`pcr_dead_writes.h`); kf4-pc draws them all. Cleared or drawn they cost the same: neither HiZ nor
  DCC off (`RADV_DEBUG=nohiz`, `nodcc`) changed them. A frame's fills left out where the frame's own walk shows them
  dead would be worth about 1.8 ms on a game frame's picture here. Radeon GPU Profiler would say why a 1920x448 fill
  takes 0.9 ms.
- **SMAA T2x's motion pass** (RG16F, written and read twice) could be worked out in the blend and the resolve, which
  read only their own pixel's: about 0.2-0.3 ms.
- **The sharpening** could be done in the presenter's copy to the swapchain, which is a blit now: one window-sized pass.
- **The brightness pass** (0.45) and its copy (0.6, "texture copy"): PC_OPTIMIZATION.md above.

## The Vega 7 again: the frame's fills (2026-11-19)

The same laptop on mains power (balanced profile), main at 25328480, the same settings and runs as above. GPU a picture
at 009: 14.16 and 14.36 ms; 2d the most of any kind, 3.4 to 3.5 ms. By shader state (`KF4_GPU_DETAIL=1`,
`--profile-at`), the most was `2d | untextured | Z always write`: 1.8 ms a picture, the frame's full-screen fills.

A game frame at 009, in order (in bold, what is written for nothing or through the shader where a clear does):

1. the colour and Z cleared (`00000080`, Z 0), cleared already (`as_clear`);
2. **the same clear again**, with the other context's registers, nothing between;
3. the reflection's 256x256 surface cleared and drawn, its Z in the frame's Z buffer;
4. **Z 0 written over the screen** with the colour masked (the alpha test NEVER, FAIL ZB_ONLY): drawn through the
   shader, exact Z and all;
5. the map, then the destination alpha's reset (ten strips, blended, Z 0 written), the hands, native 280;
6. **Z 0 written over the screen again** the same way, then the HUD and the brightness pass.

Each costs about what any layer costs on this card (memory, not the shader). Changed, both exact:

- **A Z-only write is a clear of the depth alone** (`as_clear`'s `z_only`): the depth, its stencil and the depth rule's
  copy cleared, the colour not touched.
- **A clear the same as the last is left out** (`same_as_last_clear`): the same images, rectangle and values, and
  nothing since that could have written a surface (`surface_writes_`: a batch begun, an upload, a copy, a walk drawn
  again, an image made). `KF4_PC_CLEAR_AGAIN=1` draws it (to compare). The pump's dead writes already left it out of
  the pictures between; this is the game frame's own picture.

| | 009 main | 009 branch | 020 main | 020 branch |
|---|---|---|---|---|
| GPU ms a picture (two runs each, alternated) | 14.16, 14.36 | 13.75, 13.80 | 11.65, 11.71 | 11.47, 11.55 |
| 2d | 3.44, 3.53 | 2.94, 2.94 | 3.26, 3.26 | 3.10, 3.10 |

About 0.5 ms at 009 (3.5%), 0.17 at 020: most of it the Z-only clears (2d 3.52 to 3.02 with those alone, one run);
the repeat clear about 0.1 more. Pictures byte for byte against main: 009 and 020, AA off and SMAA T2x, five
vblanks each (headless, 2x native widescreen): identical. The 37 unit tests pass.

**What is left of the fills:** the first clear and the two Z-only clears, about 0.5 ms each, and the alpha's reset
(0.45). They are partial clears: both buffers of the frame are one image (640x928 GS pixels) and the depth too, so no
clear covers a whole image and none can be a fast clear (DCC, HTILE). Each buffer an image of its own might make the
first clear nearly free (not tried); that is the next thing to look at here. The last Z-only clear (6.) is dead (nothing tests Z after it
before the next frame's clear) but the HUD writes Z after it, so leaving it out needs the frame's walk, as the pump's.


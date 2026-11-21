# The scene renderer, slice 5: results (branch `scene/slice5`, 2026-11-21)

Slice 5 of `scene-renderer-design.md` ("The slices", and "8. Scheduling and present"): the pictures by budget, never
holding the game, a repeated vblank not made again; the game's packets taken in while a picture waits for its time
(the design's drawing thread, done another way); the sharpening drawn by the presenter. On `scene/slice5` from
`scene/slice4b`.

All figures: the owner's settings in a real window (2x, a 1920 x 896 picture scaled to 1920 x 1080, FXAA, sharpening
0.5, tearing, no vsync), Vega 7, the quick save at 099 (`base.kf4quick`), 1,100 vblanks a run, `--stats`. Measurements
in `~/Documents/darker-reality-measurements/2026-11-21-scene-slice5/` (`bin/spin.sh` a run, `bin/all.sh` the set,
`bin/report.py` the table; `spin/NAME.txt` each run's log, `.turns` its late turns, `.budget` its pictures' times).

## What it does

### The pictures by budget (`port/gs/pc/pcr_budget.h`, the shell's `PictureWatch`; `KF4_SCN_BUDGET=0`: as before)

Under the scene renderer, in a run with a window (never headless, `--fast` or with `--dump-frames`: their pictures must
not depend on time), the shell times every picture it makes for the window:

- **Its cost.** kf4-pc times it from begun (the mouse's hand taken, its commands recorded) to submitted; a new thread,
  `kf4-pc-watch`, waits for its fence and times its GPU work from then, or from the GPU's work before it done if that is
  later. The budget keeps the last 32 of each kind and plans on their 90th percentile, 1.1 times over. The kinds: a game
  frame's own picture; a picture between shown by an extra picture; a picture between shown by the vblank's own (its
  phase 0: the vblank's scene is closed between its two halves, so its GPU work is in the picture's); and the vblank's
  scene closed alone, timed by an empty submit with a fence after it, ahead of the pictures after it (below, "found").
- **Begun just in time.** Each picture is begun no sooner than its cost before its time (`begin_at_s`): kf4-pc waits
  ("pc waits until a picture's time (the budget)" in the profiler), then turns the picture to the hand and draws it. A
  picture between whose next vblank's own picture is a game frame's (the frame's first vblank, the mouse not turning)
  must also leave that one room: done before the vblank's scene and the frame's picture would need the GPU.
- **Left out when it would be late.** A picture between that would not be done by its time, the GPU's queue as it is
  predicted (`GpuQueueTime`: the pictures and scenes in flight, corrected as each is seen done), is not drawn, nor the
  picture that would show it. Never waited for, never queued behind the GPU. At 60 and below a vblank's own picture left
  out shows the last picture again (the window waits a while for each vblank's picture); above 60 nothing is handed
  out (the window's thread shows the pictures by their times and waits for none). **A game frame's own picture is always
  made.**
- **gs.c asks for every picture between** (`port_gs_budgets_pictures()`): no cut, none left out as behind, and with
  `--frame-rate unlocked` every candidate of the cap (16 a vblank without one) instead of 1.25 times the rate shown, which
  the budget's own leave-outs held down (86 pictures a second; 98 with every candidate).
- **The game is never held by pictures**: the sender's only wait is still the inbox's two vblanks of the game's own
  pictures (`wait_behind_at_most`), which pictures between never count in, and which a picture begun just in time does
  not reach (30.0 game frames a second in every run below).
- Developers': `KF4_SCN_BUDGET_LOG=FILE` (a line a picture: kind, begun, submitted, done, its time, the picture before
  done, the estimates), and `--stats`' "pictures by budget" and "pictures timed" lines.

### A vblank shown again (`KF4_SCN_REPEAT=0`: every vblank made)

A vblank's own picture asked as the last one made was (the buffer shown, the picture's size, the scale, the final
pass's settings; no pixels asked for, no temporal anti-aliasing), of a buffer nothing has touched since its last
scanout (`Drawing::scanout_unchanged`: the builder forgets the last scanout wherever `touch` meets what it read or an
op writes its image, and at a scale change, a state loaded, a target grown or a picture between drawn), is that picture
again: its slot of the scanout ring published once more under the new vblank's tag, with no GPU work. At 30 that is the
second vblank of every game frame (the game draws the next frame into the other buffer); in menus and pauses, every
vblank with nothing new.

`KF4_SCN_REPEAT_CHECK=1` (developers'): each picture that would be shown again is made all the same, and the two read
back and compared byte for byte.

## Exact

- **Headless, nothing changes**: the budget is off there and the repeat never meets a picture (a headless run asks for
  none for a window). `combat`, `menus` and `watchers` of `motion_compare.py` at 2x widescreen, 60 and 240, slice 5 on and
  off (`headless/`): **0 of 801 pictures differ.**
- **4b's held frame still exact**: `tools/hold_compare.py` on `combat`, `spell 3`, `menus`, `watchers` at 2x widescreen,
  60 and 240 (`hold/`): 0 of 1,101 pictures differ.
- **The repeat**: the spin at 30 with `KF4_SCN_REPEAT_CHECK=1` (`spin/c30.txt`): **494 vblanks shown again, every one
  byte for byte the picture made again.**

## The spin and the walk (`final/spin_walk.md`)

The spin: `KF4_DIAG_SPIN=6` (6 rad/s, the mouse's own path). The walk: no mouse, the pad walking and turning
(`spin/walk.pad`). "Before": the same build with `KF4_SCN_BUDGET=0 KF4_SCN_REPEAT=0`. New pictures a second: the
pictures shown that carry a picture between's turn (`KF4_LATE_TURN_LOG`), after the first second. The late turn: what the
hand moved from the picture's turn to the presenter's recording of it (slice 4b's measure).

| | game frames a second | new pictures a second | shown > half a vblank late | sd between new (ms) | turn to the presenter, median / p90 (ms) | late yaw, median / p90 (mrad) |
|---|---|---|---|---|---|---|
| 60, the spin: before | 30.0 | 60.0 | 15 | 1.55 | 12.2 / 15.8 | 93 / 95 |
| 60, the spin: slice 5 | 30.0 | 60.0 | 13 | 1.48 | **6.6 / 6.8** | **42 / 43** |
| 240, the spin: before | 30.0 | 109.5 | 596 | 3.76 | 21.0 / 27.8 | 167 / 1,057 |
| 240, the spin: slice 5 | 30.0 | 109.6 | **21** | 2.48 | **10.2 / 10.6** | **77 / 78** |
| unlocked, the spin: before | 30.0 | 75.4 | 194 | 14.97 | 25.1 / 33.3 | 182 / 225 |
| unlocked, the spin: slice 5 | 30.0 | **98.3** | **4** | 3.66 | **10.3 / 10.9** | **70 / 72** |
| 60, the walk: before | 30.0 | 30.0 | 16 | 1.47 | 15.4 / 15.6 | |
| 60, the walk: slice 5 | 30.0 | 30.0 | 17 | 1.56 | **6.6 / 6.9** | |
| 240, the walk: before | 30.0 | 81.1 | 719 | 3.95 | 21.5 / 27.8 | |
| 240, the walk: slice 5 | 30.0 | 61.9 | **22** | 3.96 | **10.2 / 10.8** | |

(At 60 the walk's pictures between are every other vblank; the game frame's own picture is the other.)

- **The game frame rate holds** at 30 in every run, as it did under the scene renderer before (slice 4's held frame
  already kept the spin at 60 under a game frame's time); slice 5 now also holds it without the cut.
- **Every picture shown is on time.** The budget's own count (`.budget`): at 60 and 240 every picture done after its
  time was in the first two seconds (the quick load, the first frames, before any is measured); none after. Unlocked: 11
  late by more than 2 ms after the first two seconds (the worst 25 ms). Before, at 240, 596 of the pictures were shown
  more than half a vblank late, a quarter of a second's backlog at times (the late yaw's p90: 61 degrees).
- **The late turn halved at 60**: 12.2 to 6.6 ms, 5.3 to 2.4 degrees at 6 rad/s. What is left is the picture's own cost
  (0.9 ms of the CPU's and 7.3 of the GPU's, median), begun as late as it can be. At 240 and unlocked it is 10 ms (the
  budget's 90th percentile with its margin, larger there: the scene's work at each vblank lands among the pictures).
- **As many as fit**: at 240 the same 110 a second as before, now all on time; unlocked 98 a second against 75. The walk
  at 240 has fewer (62 against 81): the game frame's own picture is protected, and the pictures between that would make it
  late are left out; before, 719 pictures were shown late instead.

The budget's estimates at the end of each run (`--stats`, "pictures timed"), CPU + GPU ms with the margin: a game
frame's picture 0.1 + 3.4; a picture between 0.6-0.9 + 7.9-8.7; the vblank's scene alone 1.3 at 240.

## A repeated vblank (`spin/r30.txt`, `n30.txt`: the spin at 30, `KF4_GSN_GPU_TIME=1`)

| at 30, the spin | vblanks shown again | the renderer's GPU time in the run | a picture made |
|---|---|---|---|
| every vblank made (`KF4_SCN_REPEAT=0`) | 0 | 5,290 ms (1,096 pictures) | 4.83 ms |
| slice 5 | 494 | **3,849 ms** (602 pictures) | 6.39 ms |

**A vblank shown again costs the renderer no GPU work** (and no command buffer: its slot is published again); the run's
GPU time is 27% less, 2.9 ms for each vblank shown again (the final pass it no longer makes). The presenter's own copy of
the picture into the window is paid as for any picture (since the end of slice 5 its sharpening, below).

## Found on the way

- **A picture begun on time did not fit.** Its begin time was its deadline less its cost exactly, so a wait that woke
  a few microseconds late made it miss: at 240 only the vblank's own pictures were made (60 a second). The fit is now
  judged without the begin's slack.
- **Protecting every vblank's own picture** left at most one picture a vblank at 240 (two pictures of 8 ms do not fit
  in 16.7). Only a game frame's own picture is protected; with the mouse turning, the vblank's own picture is a picture
  between like the others and may be left out.
- **The vblank's scene is GPU work of its own.** At unlocked the pictures' GPU time had a tail (median 7.3 ms, p90
  10.8) that sat at one place in the vblank (10-14 of 16 candidates: 10.6 ms against 6.9 elsewhere): the next game frame's
  scene closed at each vblank, queued ahead of them. Timed apart (an empty submit with a fence after the close) and
  added to the GPU's queue, the pictures' own time is clean, and they are done 2.3 ms before their time (median) instead
  of 4.75.

## Finished: the packets taken ahead, and the sharpening in the presenter

### Why pictures were begun late (`why/`)

What kf4-pc was doing when a picture's time came and it was not there (the stretches of kf4-pc's time by what it did,
against each late begin, after the first two seconds; the counts first given here, 256-522 at 240, were mostly the
load's):

| after the first 2 s | pictures begun more than 1 ms late | late in all | kf4-pc meanwhile |
|---|---|---|---|
| 60 | 3 | 35 ms | taking packets in 26 ms |
| 240 | 268 | 519 ms | taking packets in 397 ms, a frame taken 96 ms, idle 20 |
| unlocked | 26 | 35 ms | recording the picture before 27 ms |

At 240 it was the build. gs.c posts all of a vblank's pictures at once and the next frame's packets queue behind them;
kf4-pc waited for each picture's time, idle, and took the packets in only after the last, when the next vblank's first
picture was already due.

### The packets taken ahead (`KF4_SCN_AHEAD=0`: not)

In place of the design's drawing thread (which would share the builder between two threads): while kf4-pc waits for a
picture between's time, it takes in the game's packets queued behind it (`Inbox::take_packet_ahead`: past pictures
between and the extra pictures that show them only; never past a vblank's own picture, a frame, a job, or gs.c's own
packets at a vblank, `GifPacket::vblanks`), each while its cost (the packets' high percentile, at least 0.5 ms) fits
before the wait's end, and waits for more meanwhile. The pictures must not see those packets, which were sent after
them:

- **The builder** (`Drawing::packets_ahead`, scn_build.cpp's `Ahead`): the scene so far is closed, and the scene the
  packets add to is closed by none of the pictures (nor its batch flushed), so the GPU draws it after them; a picture
  draws the kept frame into its stand-ins from the GS's memory as it was, and is shown with the frame's buffer placed
  and its layers as they were. Only while the pictures to come are drawn from a kept frame and shown from its stand-in
  (else refused, nothing taken ahead).
- **The shell** counts how many pictures each packet taken ahead passed; the first picture sent after them closes the
  scene first (`packets_ahead_end`), as it would in its turn. A picture between ahead of the packets keeps the intake's
  stretch (the next frame's packets, from which it is taken) instead of dropping it.

Both were found by the check below. Without them, the first version showed three black pictures between at the
teleport in `combat` (a flash), and the next frame was then not kept for its pictures between.

**Exact, checked as far as it goes**: `KF4_SCN_AHEAD_FORCE=1` (developers', headless) holds every picture between and
its extra picture back until the game's next packets are sent, which kf4-pc then takes ahead of them as above (or holds
back to their turn where the builder refuses); `tools/hold_compare.py --var KF4_SCN_AHEAD_FORCE`. Every motion case at
1x and 2x widescreen, 60 and 240, with `KF4_INTERP_T0=1` (the frames held as under the mouse) and at 2x widescreen
without: **0 of 9,834 pictures differ** (`ahead/t0`, `ahead/plain`; `combat` at 240 alone: 54,027 packets taken ahead,
none refused). The final build again on `combat`, `spell 3` and `watchers` at 240: 0 of 804 (`ahead/final`).

| the spin, real window (`spin/a*`, `n*`: `KF4_SCN_AHEAD=0`) | new pictures a second | pictures begun late after 2 s (late in all; kf4-pc taking packets meanwhile) |
|---|---|---|
| 240, not taken ahead | 100.7 | 443 (1,360 ms; 1,038 ms) |
| 240, taken ahead | **116.6** | **56** (523 ms; 289 ms) |
| unlocked, not taken ahead (three runs) | 101.3, 103.4, 103.7 | 36, 72, 117 |
| unlocked, taken ahead | **104.3, 106.2, 105.4** | 137, 152, 211 (mostly recording the picture before) |
| 60, either | 60.0 | 0-3 |

At 240, 401,862 packets (934 MB) were taken ahead in the run, none done after the wait's end, none that drew the scene
so far. The walk at 240 (`aw240`, `nw240`) makes no more pictures (60 a second either way; its pictures between are left
out for the game frame's own, not for the build) and begins 761 late against 905. At 60 nothing is queued behind a
picture between (`a60`: 0 taken ahead). The late turn and the pictures shown late are as before. (The figures before
vary from slice 5's first runs, 109.6 a second at 240 then: the runs are from another session.)

### The sharpening in the presenter (`KF4_PRESENT_FOLD=0`: not)

The renderer scales the picture into the slot and leaves it unsharpened (`GsnScanout`'s `sharpen`, `scaled_view`); the
Vulkan presenter draws `pcr_present_sharpen.frag` (an `origin` added) straight into the swapchain's image at the
picture's place, in place of its 1:1 copy, and the flash recorder takes the sharpened picture from there. A picture
whose pixels are read back (`--dump-present`, SDL_Renderer's) is sharpened by the renderer as before.

- **Exact**: `KF4_PRESENT_FOLD_CHECK=1` (developers') has the renderer sharpen each picture as before into a check
  image as well, and the presenter compares every 15th picture's pixels in the window with it: **0 of 100 differ** in
  the spin at 240 (`spin/foldcheck`), 0 of 20 at 60 (`fold/check60`).
- **Smaller than estimated**: 0.13 ms a picture, not 0.5. The renderer's sharpening pass goes (6.85 to 6.19 ms a
  picture at 240), but the presenter's draw into the swapchain's image costs more than its copy did (1.17 against 0.63
  ms with the flash recorder, 0.91 against 0.40 without): writing the window's image from a shader costs about 0.25 ms
  more than writing the renderer's own image. At 60 likewise 0.14. The budget's estimate of a picture between's GPU
  time falls from 8.2 to 7.3 ms, and 117.8 pictures a second fit at 240 against 115.2 (`spin/gpu_fold`, `gpu_nofold`).

## Not built

- **The design's drawing thread**: replaced by the packets taken ahead, which take the build out of the pictures' way at
  240 without a second thread on the builder. What neither does: a picture whose frame's packets are not yet sent
  waits for them.
- **The reprojection** (decision 3, moved after slice 5 by 4b). The late turn left is the picture's own cost: 6.0-6.6 ms
  at 60, 2.4 degrees at 6 rad/s, about 40 mrad (16 GS pixels). A reprojection at the presenter would take most of it,
  but it moves the HUD with the world unless the HUD is an image of its own (decision 4); a cheaper picture (3f: the
  GS's pixel in the shader, 0x700 placed) takes it down for every picture and lets more fit at 240. The second is the
  next step; the reprojection can be judged after it.

## The criteria

| slice 5 passes when | measured | |
|---|---|---|
| the mouse spin at the owner's settings holds 30 game frames a second | 30.0 at 60 (and at 240 and unlocked) | yes |
| ... with as many pictures between as fit | 60 a second at 60; 117 at 240 (101 with the packets not taken ahead) and 104-106 unlocked, on time (before slice 5: 110 at 240, 596 late) | yes |
| a picture's GPU time, the final pass with the presenter | the presenter sharpens straight into the window: 0.13 ms a picture less, byte for byte | yes |
| a repeated vblank under 0.3 ms | no GPU work in the renderer (the presenter's sharpening as for any picture) | yes |
| (4b) the late turn after slice 5's scheduling, sized again | 6.6 ms at 60, 2.4 degrees at 6 rad/s: the picture's own cost | measured |

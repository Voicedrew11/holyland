# The scene renderer, slice 5: results (branch `scene/slice5`, 2026-11-21)

Slice 5 of `scene-renderer-design.md` ("The slices", and "8. Scheduling and present"): the pictures by budget, never
holding the game, a repeated vblank not made again. On `scene/slice5` from `scene/slice4b`.

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
the picture into the swapchain is paid as for any picture (about 0.5 ms; `presenter-sharpen-into-swapchain` below).

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

## What is not built, and what it would give

- **The drawing thread** (the design's "a drawing thread records and submits every picture"). kf4-pc builds the scenes
  and draws the pictures, waiting for each picture's time between. At 60 that is enough (28-30 pictures of about 1,060
  begun more than a millisecond after their time); at 240, 256-522 of about 3,500 were, kf4-pc still taking a scene's
  packets in when a picture's time came, and some of those were left out for it. A thread of its own would let them be
  begun on time; it needs the builder's scenes and images shared with it (the builder is one thread's today). Worth it
  once the pictures are cheap enough for more of them to fit.
- **The final pass in one with the presenter**: the presenter's 1:1 copy into the swapchain (about 0.5 ms a picture,
  every picture, a repeated one too), the sharpening drawn by the presenter into the swapchain image instead
  (`scene-final-pass-results.md`, "What is left").
- **The reprojection** (decision 3, moved after slice 5 by 4b). The late turn left is the picture's own cost: 6.6 ms at
  60, 2.4 degrees at 6 rad/s, 42 mrad (16 GS pixels). A reprojection at the presenter would take most of it, but it moves
  the HUD with the world unless the HUD is an image of its own (decision 4); a cheaper picture (3f: the GS's pixel in the
  shader, 0x700 placed) takes it down for every picture and lets more fit at 240. The second is the next step; the
  reprojection can be judged after it.

## The criteria

| slice 5 passes when | measured | |
|---|---|---|
| the mouse spin at the owner's settings holds 30 game frames a second | 30.0 at 60 (and at 240 and unlocked) | yes |
| ... with as many pictures between as fit | 60 a second at 60; 110 at 240 and 98 unlocked, all on time (before: as many at 240, 596 late) | yes |
| a repeated vblank under 0.3 ms | no GPU work in the renderer (the presenter's copy as for any picture) | yes |
| (4b) the late turn after slice 5's scheduling, sized again | 6.6 ms at 60, 2.4 degrees at 6 rad/s: the picture's own cost | measured |

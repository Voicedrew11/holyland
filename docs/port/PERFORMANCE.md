# Performance: what limits the game, and what to do next

Measured 2026-11-05 on branch `pc/renderer` (76491c12), the owner's machine (Ryzen 7 5700X, RX 9070 XT, RADV).
Runs: `--renderer pc` at the owner's picture settings (4x, native widescreen, MSAA 1, aniso 16, cut-out AA ssaa,
native draw on), `--frame-rate 240`, `KF4_PRESENT_SIM=240`, `tests/newgame.pad` + `--teleport 1900:MAP:0
--invincible`, headless. Every thread sampled with `eu-stack -i -p PID` (200 samples at 009).

## What was measured

| | |
|---|---|
| map 000 | 239.75 pictures a second (the cap) |
| map 009 | 137 a second, lowest second 117: below a 170 Hz screen |
| pc's thread (`kf4-pc`) at 009 | 97% busy: 45% drawing the pictures between again (`draw_again`), 41% taking the frame's packets in (`GifFrontEnd::feed`) |
| of it, Granite's draw calls and descriptor sets | about 23% (`CommandBuffer::draw`, `flush_descriptor_sets`, `radv_CmdDraw`) |
| the GPU | 2.4-2.7 ms a picture |
| the game's thread | about 5.4 ms of work a game frame (30 a second); asleep in `sceGsSyncV` the rest: about 6x room |
| the DMA thread | about a third busy, mostly copying packets (`memmove`) |
| loads (teleport) | about 25 ms; no stalls in play |
| 70 s at 009 | 11 GB of GIF packets, 55 GB of uploads through the texture store, 773,093 uploads asked for again by the pictures between, 6.5M CLUT loads |

**Note:** `fast` (the owner's `settings.json`) is still the native GS's path; the PC renderer runs only as `pc`
until step 7 switches `fast` to it.

## Measured again after the pump (2026-11-06): what each picture waits on

At 305b216a (the pump), 009 at 240 made 148-154 pictures a second (lowest seconds 127-132), with both pc threads
mostly idle. A thread's busy share does not say what the rate waits on, so this time each picture was followed through
the threads with timestamps (a diagnostic build, not committed: the game's vblank and its posting, the DMA thread's
vsync and packets, the picture asked for, taken by kf4-pc, submitted, the GPU done (a fence waited on by a thread of its
own), handed to the presenter; and Granite's real queue submits), and with `perf record --switch-events` for each
thread's time on and off the CPU (`nix build nixpkgs#linuxPackages.perf`; `perf sched` and off-CPU stacks need root
here: tracefs is root's, and a context switch's stack is in the kernel).

**What it showed.**
1. **Every second vblank had no pictures between at all** (628 of 1199 vblanks: the presenter showed all it was
   given, 155 a second). gs.c left out the odd vblank's three: `gsr_backlog() > 1`, checked after the vblank's own
   picture is sent, so it read "the vblank before is not handed out yet". The game posts a vblank about 16 ms before
   its time; kf4-pc hands out the even vblank's own picture after the frame's 7.9 MB of packets, which the DMA thread
   has sent before it, at about its time + 0.5 ms, and the odd vblank is posted at its time + 0.26 ms: missed by a
   third of a millisecond, though the window shows that picture only at its time + 4.2 ms (THREAD_DELAY). gs.c's
   own count of these was never printed with `--renderer pc` (pc's stats returned first), which is why the pump's
   notes found "none left out".
2. **The pump's pictures reached the GPU a vblank late.** Granite holds a submit without a fence or semaphore until
   the next frame context, which only kf4-pc moves on (at its own pictures). The pump's pictures were on the GPU's
   queue a median 17 ms (p90 28 ms) after they were handed to the presenter: in a window the presenter could take a
   slot not yet drawn. Headless runs, which hand the presenter only the tag, could not show it.
3. Not limits: Granite's frame contexts (kf4-pc waited in `next_frame_context` 0.02-0.23% of play); the sender's waits
   for room and for the renderer two vblanks behind (none); the pictures between asked all at once at the vblank
   (they are made in order by the pump, which keeps up).

**What was done** (`gsr_behind`, gs_api.h; `submit_now` in PictureOut and `Canvas::submit`, pcr.cpp and
pcr_canvas.cpp; gs.c's left-out line printed for pc too):
- pc is behind when the vblank before the one before is not handed out (`pcr_backlog() > 2`): a picture then is past
  its time in the window. The native GS keeps its test (`> 1`, unchanged).
- Every pc submit is put on the GPU's queue at once (`Device::flush_frame` after it).
- Found by TSan on the way (from the pump, 305b216a): the pump read its GPU timestamps' results while kf4-pc's
  `next_frame_context` wrote them (Granite's `QueryPool::begin`); the pump now collects them under Granite's device
  lock (`external_queue_lock`).

**Checks.** Pictures byte for byte against 3964bb4b at 000, 009, 009:2 and 063 at 240 (`KF4_INTERP_DRAW_ALL=1`,
vblanks 2300-2650 every 7th: 204 a spot). ctest 14/14. ASan + UBSan pc run at 009:2: only the known reports. TSan pc
run at 009: 269 reports, none in pc's code; in renderer code only the 4 known ones in Granite's image making at start
(the two canvases made at once). Not run: native and `fast` checks (their paths are not changed: the native GS's test
is as before), clang-tidy (no configuration).

**After** (`pace_test --hz 240 --frame-rate 240 --maps 0,9 -- --renderer pc`, against 3964bb4b, runs alternated):

| | 009 pictures a second (lowest) | 009 uneven refreshes a second | 000 pictures a second (lowest) | 000 uneven |
|---|---|---|---|---|
| before | 150.1 (115), 160.4 (137), 158.8 (128) | 134.2, 119.8, 125.0 | 235.1 (221), 212.1 (201), 215.1 (197) | 6.6, 36.8, 33.0 |
| after | 239.6 (238), 239.6 (238), 239.7 (238) | 21.5, 19.0, 25.8 | 239.75 (239) each | 1.8, 1.4, 0.5 |

None left out. The GPU 85% busy at
2.2-2.3 GHz (before 72% at 1.65 GHz: `gpu_busy_percent`, `pp_dpm_sclk`). GPU done after a pump picture's last submit:
0.8-1.1 ms (3.4 before the flush, with a fence per picture).

**What limits it next: kf4-pc's intake, then the GPU.** The DMA thread has sent the whole frame 3.8 ms after the odd
vblank; kf4-pc takes the even vblank's own picture 12-16 ms after the frame's last packet (it takes the packets in
first), and the pump starts the frame's pictures between only after that. So the pictures done on the GPU later than
the window's allowance (their time + 4.2 ms) were 3% to 58% from run to run with the same build (bimodal: kf4-pc's
intake 11.9 against 15.5 ms at the median; the game's and the DMA thread's timing the same in both). Not kept:
submitting the intake every 128 batches as well (13% and 28% late against 3% and 58% without: within that noise). The
pump is 77% busy at 240; each picture between ends about 13 render passes, each with a full barrier
(`Canvas::end_pass`).

## The list, best first

0. **Done 2026-11-06** (above): the pictures between of every second vblank, and the pump's pictures on the GPU's
   queue when handed out.

1. **pc's work on two threads.** Done (305b216a, the pump; PC_RENDERER.md "The pictures between on a thread of
   their own").
2. **Fewer draw calls and descriptor writes.** About 23% of pc's thread is Granite's per-draw cost: merged draws
   (item 4 of the handoff), bindless textures for draws across texture and CLUT changes. Since 2026-11-06 this is
   what makes pictures late (kf4-pc's intake, 12-16 ms a frame after its last packet), and the pump's 3.2 ms of CPU
   a picture between is the same per-draw cost again. Then the GPU (85% busy at 240): the full barrier after every
   render pass.
3. **Uploads not sent and checked again.** The pictures between ask again for uploads the store already holds;
   each is hashed and compared again. Refer to the kept chunk instead. CLUT loads are another 11% (a key vector and
   512 mirror writes each).
4. **`fast` switched to `pc` (step 7)**, with the settings pc lacks (MSAA 2, `--native-colour high` and the
   others): the owner's MSAA 2 and colour high raise pc's GPU time from today's 2.7 ms a picture.
5. **A release build.** The presets are `RelWithDebInfo` (-O2, no LTO). Try LTO, then PGO.
6. **The game's thread lighter** (for slower CPUs; not a limit here):
   - About half its work is exact VU0 float maths, mostly the CPU's near cut of map records
     (`ModelScreen_CutClippedTriangles` → `Vu0_ProjectToScreen3`), still run when a record is drawn natively.
   - About 4.8 MB of meshes hashed each game frame (`rd_mesh_from_block`; smooth play step 5, item 2): hash only
     what changed, or a faster hash.
7. **Small ones.**
   - `KF4_HITCH_MS` times from one vblank's start to the next, so it counts the game's own work: every other vblank
     shows about 22 ms at a threshold of 20. Time how late each vblank is against when it was due.
   - The pictures-between thread (`show_until`, video.c) spins up to 0.5 ms before each picture.
   - Door and area loads from a disc not in the page cache: not measured (smooth play step 5, item 1).
   - Not measured: Windows, NVIDIA, other GPUs.

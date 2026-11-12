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

## Profiled 2026-11-07: which functions (the in-game profiler)

At 4a2932e7, with the profiler (docs/port/PROFILING.md): `tools/sampler/profile.py --map 9 --hz 240 --seconds 20
-- --renderer pc --frame-rate 240` (the owner's picture settings, `KF4_INTERP_CUT=0`), and a second run with
`--gpu-detail` for the GPU by shader state. Shares below are of that thread's CPU time; "self" is the function's own
code, "incl." with what it calls. Read again with `stacks.py ~/.cache/kf4p9/samples.txt out/linux-clang/kf4`.

**Where 009 stands.** At 240 it holds the cap (239.6 a second). Uncapped (`--hz 500`) it makes 238-250 a second and
the game falls behind (56-58 vblanks a second): about 240 is the ceiling today. CPU in 20 s of play: the pump
(`kf4-pc-pump`) 16.0 s (80% of a core), `kf4-pc` 12.6 s (63%), `kf4-dma` 4.0 s, the game's thread 3.7 s,
`kf4-between` 2.0 s. The GPU: 3.16 ms a picture, three quarters of the 4.17 ms a picture has at 240.

**The pump is the busiest thread now**, and almost all of it is drawing the kept batches again (`draw_again_alone`
97%, `draw_batch` 76%). Its own code is small; what it pays for is per batch:

| self | what | from |
|---|---|---|
| 13.4% | `release_reference` (Granite's atomic reference counts) | `~BufferBlockAllocation` 7.3% (each batch's vertex and constant blocks let go), `flush_batch`'s `reset` 5.2% |
| 17.5% incl. | `radv_CmdDraw` and the state it emits | the driver |
| 10.0% incl. | `TextureStore::drawn` marking pages (`changed`'s bitset 6.1%) | every batch drawn again marks its pages again |
| 8.7% | the kernel submit (`ioctl` in `ac_drm_cs_submit_raw2`) | one queue submit a picture between, since every pc submit is flushed at once (2026-11-06) |
| 6.9% | `memcmp` in Granite's `bind_pipeline` | a pipeline change at most batches: native 280 alternates between two blend states about 900 times a picture |
| 6.2% incl. | `radv_UpdateDescriptorSetWithTemplate` | a descriptor set a batch |

**`kf4-pc`'s intake:**

| self / incl. | what | from |
|---|---|---|
| 14.7% incl. | copying `std::variant` kept steps (`_Copy_ctor_base` in `construct_at`) | `Kept::steps` is a vector of `KeptStep`, a variant as large as its largest, `Batch` (a 1 KB CLUT inline): every step, a 16-byte `KeptClutLoad` too, takes a `Batch`'s room, and the vector's growth copies them (the move is not used) |
| 11.8% | `memcpy` | the kept walk's copies (`as_picture_walk`, the variant above), `image_data`'s inserts, `shared_ptr` byte copies |
| about 14% | exact VU float maths (`vu_float_round`, `_add`, `_value`) | the CPU half of native 280 (`clipped`, `vu_add`: `draw_280` is 38% incl.) |
| 5.9% incl. | `hash_bytes` (`mix`) | `TextureStore::keep` hashing uploads |

**The GPU at 009, by shader state** (detail on; the kinds' total agrees within 1%):

| ms a picture | runs a picture | state |
|---|---|---|
| 1.075 | 438 | native 280, sampled rgb, fog, blend (Cs-Cd)*As+Cd, Z >= write: the map |
| 0.351 | 5 | 2D untextured, blend (Cs-Cd)*As+Cd: a few large blended rectangles (not looked at yet) |
| 0.221 + 0.206 | 59 + 451 | native 280, sampled rgba, the two other blends: the state the map alternates with |
| 0.210 | 3 | 2D untextured, Z always write: probably clears drawn as rectangles (not looked at yet) |
| 0.300 | | native 700 (two states) |

## Built 2026-11-07: batches drawn where they are kept, and the GPU's memory at uncapped rates

From the list below: item 1's **pages marked again** and item 2's **kept steps out of the variant**, and with them
smaller per-batch copies (`pcr_canvas.cpp`):
- A kept walk holds its batches in a vector of their own (`Kept::batches`); a step is a `KeptBatch` index, so a
  `KeptStep` is small, the kept types move (non-throwing), and a walk reserves the last walk's sizes.
- `draw_batch(const Batch &)` draws a batch where it is: the open one, or a kept walk's again without a copy (a copy
  only for a surface's texture, which is made again). A frame inside a surface moves its vertices as they are copied
  to the GPU, not in the batch; a moved batch's box is a local.
- A batch that reads no CLUT binds one shared buffer instead of 1 KB of constant data a batch.
- The pump's canvas no longer marks the pages it draws over in its texture store, which nothing reads
  (`store_follows_draws_`; never-uploaded pages answer NotUploaded with or without marks).

At 009, 240, 20 s profiled: the pump 15.7 s of CPU to 10.5 s (-33%), `kf4-pc` 12.0 s to 9.4 s (-22%), the same
pictures; the variant copies (about 10% of `kf4-pc`) are gone. Pictures byte for byte at 000, 009, 009:2, 063.

**A GPU memory leak at uncapped rates, found by it** (it logged the owner out of the desktop twice): at `--hz 500` the
pump makes most pictures, and only kf4-pc moves Granite's frame contexts on (at the pictures it makes). Granite
frees what a thread lets go of (each picture's vertex and constant blocks, command buffers) only as its frame
contexts come round, so the pump's were never freed: about 6 MB a picture, 2.5 GB a second, until the GPU's 16 GB and
then the system's memory were full and KWin lost its framebuffer. At 240 kf4-pc made enough pictures to keep them
turning. Now kf4-pc moves the frame context on for the pump's pictures too (`take_picture`), and kf4-pc waits before
sending the pump a frame while one sent is not yet started (`Pump::wait_for_room_for_frame`: each holds a copy of every
surface). At 500 the game's GPU memory now levels off about 2.5-3 GB, as at 240. `systemd-run -p MemoryMax` does not
cap GPU memory: run game runs with a watchdog on `/sys/class/drm/card*/device/mem_info_vram_used`.

`pace_test.py` at 009, 3 runs each, alternated (the old build cannot run uncapped):

| | 4a2932e7 + docs (32601ee2) | now |
|---|---|---|
| 240: pictures a second | 238.3-239.7 (lowest second 229-239) | 239.7-239.75 (237-239) |
| 240: uneven refreshes a second | 25-32 | 0.6-1.8 |
| 500 (uncapped) | fills the GPU's memory | 352-364 (lowest second 300-349), the game 57-58 vblanks a second |

The smoother 240 comes from the frame contexts moved on at the pump's pictures (Granite's deferred work done
evenly). Uncapped, the game now falls behind a little (57-58 vblanks a second): the next limit.

## Built 2026-11-07 (later): no pipeline binds between blends, and a walk's vertices on the GPU once

Profiled again first (009 at 240, after the two commits above): the pump's reference counts were down to 4.6% (one
a GIF batch, the vertex block's handle in `allocate_vertex_data`); its pipeline binds 11%, of which the `memcmp` is
RADV's own (`radv_CmdBindPipeline`, called from Granite's `bind_pipeline`), not Granite's state compare.

- **Dynamic state** (`Canvas::Impl::choose_dynamic`): pc's command buffers take the native GS's Granite patch
  (cmake/granite_dynamic_state.cmake): depth, stencil, culling and blend constants dynamic with Vulkan 1.3, blend
  enable, equation and write mask too with VK_EXT_extended_dynamic_state3. Native 280's two blend states then share
  a pipeline: the binds go (12% of the pump), RADV's PS epilog lookups for the dynamic blend come (about 8%:
  `lookup_ps_epilog`, `radv_generate_ps_epilog_key`). `KF4_PC_DYNAMIC=0`: none; 1: not the blending.
- **A kept walk's vertices on the GPU once** (`put_vertices_on_gpu`, `Kept::vertices`, `Batch::gpu_first`): when a
  walk is handed out (`take_kept_walk`, `as_picture_walk`) its GIF batches' vertices go into one buffer of its own
  (host-visible; without such memory, copied at each draw as before). Drawn again, a batch binds its place there:
  no allocation, copy or reference count a batch. A frame inside a surface no longer has its vertices moved on the
  CPU: `pcr_prim.vert` adds `PrimPush::place`, as `place_of` already did for moved corners (so every batch, the
  open one too, binds its vertices once for its passes). `KF4_PC_WALK_VERTICES=0`: copied at each draw.

009 at 240, 20 s profiled: the pump 10.7 s of CPU to 9.3 s (-13%; dynamic state alone 10.8 to 10.0);
`allocate_vertex_data`, `release_reference` and `bind_pipeline` are out of its top 100. GPU 3.1-3.2 ms a picture,
the same. `pace_test.py` at 009 (alternated against 16069b27):

| | 16069b27 | now |
|---|---|---|
| 240: pictures a second / uneven refreshes | 239.75 each / 0.6-1.6 | 239.75 each / 1.1-2.7 |
| 500: pictures a second | 356.5, 356.7, 347.2, 351.4, 352.9, 348.3 (mean 352) | 368.5, 360.2, 356.0, 350.8, 353.0 (mean 358) |
| 500: game vblanks a second | 56.6-58.3 | 56.8-58.8 |

Uncapped, about 5 a second more, within the runs' noise; dynamic state alone measured none (351-353 either way).
**The pump is not what limits 500**: there it is 77% busy, kf4-pc 48%, the GPU about 80%, and the game still
falls to 57-58 vblanks a second. The limit is how the threads wait on each other: next, a per-picture timeline at
500 (the diagnostic patch from 2026-11-06, ~/.cache/kf4tl) to find what the game's vblank waits on.

**Checks.** Pictures byte for byte at 000, 009, 009:2, 063 (816), and with both switches off at 000 and 009:2. ctest
15/15. ASan+UBSan pc runs at 000 and 009:2: only the known reports. TSan pc runs: the known ones only (in pc's code
only Granite's and RADV's image making in the canvas's constructor). Not run: native and `fast` checks (their
paths unchanged: the Granite patch was already theirs, gsn_shaders.h made again the same), clang-tidy (no
configuration).

`pace_test.py` now deletes its temporary folder at the end (about 400 MB a run in /tmp; 25 runs filled its quota)
unless `--keep` names one.

## The list, best first

0. **Done 2026-11-07**: item 1's reference counts and pipeline changes (dynamic state, a walk's vertices on the GPU
   once), above: the pump 13% less CPU, the uncapped rate about the same.
   **Done 2026-11-07**: pages marked again (item 1) and kept steps out of the variant (item 2), above.
   **Done 2026-11-06**: the pictures between of every second vblank, and the pump's pictures on the GPU's queue when
   handed out. **Done 2026-11-07**: the in-game profiler (docs/port/PROFILING.md), which the items below come from.
   The pump (the old item 1) is done too (305b216a).

1. **What limits the uncapped rate** (new, 2026-11-07): at 500 no thread is full (the pump 77%, kf4-pc 48%, the GPU
   about 80%) and the game falls to 57-58 vblanks a second. Follow each picture and vblank through the threads
   (the 2026-11-06 timeline) to find what the game's vblank waits on, before more per-thread work.
   **The pump's cost a batch** (less than measured above: the pump is 9.3 s of CPU in 20 s at 240 now):
   - **One submit a picture between** (9%, the kernel's submit): the price of the 2026-11-06 flush. Measure the
     pictures' lateness with two pictures between a submit before trading any back.
   - Then the old item 2: **merged draws and bindless textures** for the rest of the per-draw and descriptor cost
     (descriptor writes 6%, the draws themselves).
2. **`kf4-pc`'s intake** (63%; its lateness made pictures late on 2026-11-06):
   - **Kept steps out of the variant.** Keep `Batch` out of line (a vector of batches, or a `unique_ptr`) so a
     `KeptStep` is small; give the kept types non-throwing moves so the vector's growth moves; reserve the frame's
     steps from the last frame's count. About 15% of `kf4-pc` and part of its 12% `memcpy`.
   - **Uploads not sent and checked again** (the old item 3): `keep`'s hashing is 6%; the pictures between ask again
     for uploads the store holds. CLUT loads: `load_clut` and `clut_colours` 3-5%.
   - **The VU maths for native 280** (about 14%): exact EE float rounding (`vu_float_round`) for each vertex in
     `clipped`. A wider exact path (four lanes at once) or skipping the exact rounding where the result cannot
     differ.
3. **The GPU** (3.16 ms a picture at 009: room for about 315 a second, less with the owner's MSAA 2 and colour high):
   - Look at the **2D untextured blended rectangles** (0.35 ms in about 5 a picture) and the **2D Z-always
     rectangles** (0.21 ms in 3: if they are clears, `vkCmdClearAttachments` or a pass's load op).
   - The native 280 map state (1.08 ms): the full barrier after every render pass (`Canvas::end_pass`), then the
     shader itself (RGP: `KF4_GPU_LABELS=1`, PROFILING.md).
4. **`fast` switched to `pc` (step 7)**, with the settings pc lacks (MSAA 2, `--native-colour high` and the
   others): they raise pc's GPU time from today's 3.2 ms a picture at 009, so item 3 comes first.
5. **A release build.** **Built 2026-11-11, speed not yet measured on a quiet machine.** The `*-release` presets
   (`RelWithDebInfo` -O2, `KF4_FRAME_POINTERS=OFF`, `KF4_LTO=ON`: ThinLTO with clang) and `tools/pgo_train.py` (PGO
   trained at 000, 009, 009:2, 063); how-to in `tools/README.md`, "A release build". LTO leaves the game's objects,
   ico-audio's and the state markers alone (the save state's image, and the game's float flags); PGO covers
   everything. Both give the same pictures, state lines and F10 dump as `windows-clang` at the four spots, byte for
   byte (`tools/build_compare.py`), and the same unit test results (Windows; Linux clang and gcc: the CPU tests).
   Frame pointers were already off on Windows, so there "release" is the developers' build plus LTO.
   Measured with other load on the machine (a game playing, other agents' tests), `tools/build_cost.py` at 009, 240,
   3 runs each alternated, CPU ms a second of play (the game's thread without its spin in `port_dmac_wait`):

   | thread | windows-clang | + LTO | + LTO + PGO |
   |---|---|---|---|
   | `kf4` (the game's), its work | 242 ±14 | 206 ±22 | 207 ±24 |
   | `kf4-pc` | 633 ±59 | 574 ±117 | 584 ±83 |
   | `kf4-pc-pump` | 318 ±32 | 318 ±95 | 336 ±122 |
   | `kf4-dma` | 252 ±22 | 226 ±47 | 222 ±42 |

   Only the game's thread looks lighter (about 15%; its own code is compiled as before, so it would be the port code
   it calls: `port/math/libvu0.c`, the PS2 libm, the DMA hand-off), and that figure is an estimate from samples (the
   share of its stacks in the spin); the rest is within the noise. Left: the same on a quiet machine (`build_cost.py --runs 5`, and
   `pace_test.py --hz 500` uncapped), Linux (`linux-clang-release`, `pgo_train.py` there), and whether PGO earns its
   extra build step.
6. **The game's thread lighter** (for slower CPUs; 19% of a core here, not a limit):
   - About half its work is exact VU0 float maths, mostly the CPU's near cut of map records
     (`ModelScreen_CutClippedTriangles` → `Vu0_ProjectToScreen3`), still run when a record is drawn natively.
   - About 4.8 MB of meshes hashed each game frame (`rd_mesh_from_block`; smooth play step 5, item 2): hash only
     what changed, or a faster hash.
7. **Small ones.**
   - `KF4_HITCH_MS` times from one vblank's start to the next, so it counts the game's own work: every other vblank
     shows about 22 ms at a threshold of 20. Time how late each vblank is against when it was due.
   - The pictures-between thread (`kf4-between`, `show_until`, video.c) spins up to 0.5 ms before each picture: 10%
     of a core at 240.
   - Door and area loads from a disc not in the page cache: not measured (smooth play step 5, item 1).
   - Not measured: Windows, NVIDIA, other GPUs.

Measure each with `pace_test.py` (uncapped, `--hz 500`, alternated, 3+ runs: runs differ by about 5 a second) and
the profiler's share of the thread it targets.

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

## Found 2026-11-12: the game's 57-58 vblanks a second uncapped were vblanks left off the screen (#107)

Windows, RTX 5080, ae9a9097, Andrew's profile (`pace_test.py`'s OWNER) at map 000, `--hz 500`. A per-vblank log (a
diagnostic build, not committed: each `port_gs_run_vsync`'s start, the backlog, the pictures asked and left out and
why; each vblank's posting and due time from the game's thread) showed:

- **The game keeps its speed.** Its vblanks are posted 11-16 ms before their time and handed to the PC renderer
  within 0.3 ms (`run_vsync`); 59.9 vblanks a second over the 25 s, uncapped as at 240. Nothing on the game's side
  waits.
- **pace_test's "game vblanks a second" counts the vblanks with a picture on the screen**, and 64-68 of 1497 had
  none. At 500 a vblank's own picture is not one of its phases (8-9 pictures between at 0.12 steps from 0.01-0.1),
  so its pictures between are all it has; gs.c left out every one of them while the renderer was behind
  (`gsr_behind`: `pcr_backlog() > 2`, tested per picture after the vblank's own message is sent). Every such vblank
  had started with a backlog of 2 (the two before not handed out): all of a vblank's pictures were left out, never
  some.
- **That also kept the cut from working.** The cut judges two vblanks in a row starting with a backlog over 1; the
  whole vblank left out cleared the backlog first, so it cut seldom and never below 7 a vblank (allowed 7: 870
  vblanks, 8: 462, 9: 165) while the renderer made about 6 a vblank (368 a second).

**Fixed** (gs.c, `port_gs_run_vsync`): a vblank whose own picture is not shown keeps at least one picture between:
when every one would be left out as behind, the last of them is drawn anyway (still in order: none of the vblank
after it). `--stats` counts them ("N vblanks behind kept one"); `KF4_INTERP_KEEP_ONE=0` leaves them out as before.
In the same run: 0 vblanks without a picture (64 with the switch off), the pump's pictures 18,042 against 18,013.
Pictures byte for byte the base build's at 000, 009, 009:2, 099 (240, `KF4_INTERP_DRAW_ALL=1`, 204 a spot: dumps do
not take this path). ctest's 16 tests pass.

`pace_test` (25 s), alternated against ae9a9097, 3 rounds; **the display was asleep in all three** (GPU ~2160 MHz:
the baseline's "display off" state, perf-baseline-1112), so compare within the table. Median [min-max]:

| profile | rate | spot | pictures a second, base → now | game vblanks a second, base → now |
|---|---|---|---|---|
| andrew | 500 | 000 | 322.5 [249.1-331.7] → 328.8 [325.0-329.5] | 58.3 [58.0-58.7] → **60.4** [60.2-60.4] |
| andrew | 500 | 009 | 267.3 [204.2-277.8] → 277.0 [276.5-282.8] | 58.6 [56.7-58.9] → **60.6** [60.3-60.6] |
| sean | 500 | 000 | 341.4 [338.6-350.9] → 358.1 [349.2-387.1] | 60.6 (reached the rate) → 60.8 |
| sean | 500 | 009 | 373.9 [366.8-377.5] → 375.1 [374.2-377.2] | 58.7 [57.9-59.0] → **60.7** [60.6-60.8] |
| both | 240 | 000, 009 | 236.0-239.8 → 237.9-239.8 | 60.6-60.8 → 60.6-60.8 |

(The base's lowest rates, 249 and 204 at 500 and 207.7 at 240 for 009, are all from round 0, where it ran first.)
Uneven refreshes a second are the same at 500 (andrew 000: 324 → 320; sean 009: 248 → 247).

**What is left:** the picture rate itself. Uncapped, the renderer still makes ~6 pictures a vblank against 8-9 asked
with neither the pump nor the GPU full; about 1,000 pictures between a run are still left out as behind, some
vblanks then show one picture, and the cut stays at 7-9 a vblank (it still judges on a backlog the left-outs keep
low: judging on pictures left out too, as `--frame-rate unlocked` does, would make it hold ~6 evenly, at the cost of
a few pictures a second). The backlog at a vblank's start is 1 most of the time: the pictures of a game frame wait
for kf4-pc's intake before the pump starts them (2026-11-06), so the next step is the timeline of a frame's
pictures between (asked → pump start → GPU done), not the vblank.

## Built 2026-11-12 (afternoon): a kept walk's CLUTs and textures bound once (#110, steps 1-2 of the design below)

Branch `perf/bindless`, two commits, the design's first two steps ("Designed, not built" below):
1. **A walk's CLUTs in one buffer of the walk's** (`put_cluts_on_gpu`): its batches' CLUTs (the same CLUT once) are put
   in a storage buffer when the walk is handed out, as its vertices are; its pictures between read them there (set 0,
   binding 15, `u_walk_cluts`, at `PrimPush::clut_at`) instead of a 1 KB copy into a set 1 of its own at each draw.
   **Worth almost nothing at the measured spots**: a CLUT is read by the shader only for a texture the GPU's sampler
   cannot read (a region mode, a texture decoded in part), and at 009 there are none (`--stats`: 514,208 of 547,112
   batches sampled, 0 by the GS's read); the sampler's textures have their CLUT applied when decoded. Kept because
   step 2 needs a CLUT that is not in set 1, and other maps do read by the GS.
2. **A walk's textures in one descriptor table of the walk's** (`put_table_on_gpu`, `make_table`): every image its
   batches read that the walk holds (decoded and sampled textures) gets a place in a bindless table (Granite's
   bindless set: set 2, binding 0, an unsized array of sampled images; `u_table` and `u_table_uint` alias it, colours
   and integer texels), written once when the walk is handed out; its pool is the walk's and goes with it. The
   samplers go in a 16-entry array in set 0 (bindings 16-31, `u_samplers`, the stock nearest one first, 15 of the
   walk's; a batch whose sampler finds no room keeps set 1's). A batch's places are in `PrimPush::table`. Its
   pictures between leave set 1 as it is (the dummies) and bind the table once: **no descriptor set is allocated or
   written a batch** (set 0 still at each render pass, as before). A surface's texture and the destination copies
   (shader blends, DATE by a copy) stay on set 1, made at the draw. kf4-pc's own frame draws bind a one-dummy table
   and read set 1 as before. Granite needed no patch. The device must have `descriptorIndexing` (Vulkan 1.2's; every
   desktop driver): without it the batch programs' layout fails, as no fallback was built.

`KF4_PC_BINDLESS=0` undoes both (to compare). `--stats`' pump line counts the walks' textured batches and how many
read the table (Andrew's 009 at 500, 2470 vblanks: 3,746,274 textured batches, all from the table, none with a walk's CLUT).

**Checks.** Pictures byte for byte origin/main's (391e27cf) at 000, 009, 009:2, 099 at 240 with every picture between
(`KF4_INTERP_DRAW_ALL=1`, vblanks 2400-2403 and 2460, 20 pictures a spot), 1x and 4x MSAA 2, after each step (160
each); the title and menus (`tests/newgame.pad`, vblanks 300-1800) byte for byte. The validation layer with sync
validation silent at the four spots, 1x and 4x MSAA 2, and with `KF4_PC_PUSH_DESCRIPTORS=1` at 000 and 009 (1x).
Unit tests 16/16 (Windows). Not run: Linux/RADV, `KF4_PC_PUMP=0` (kf4-pc drawing the walks itself: the same code
path, `draw_again`).

**A/B** (origin/main 391e27cf against the branch; Windows, RTX 5080, **display off** in every round (owner at work),
builds alternated). Pump CPU (`build_cost.py`, 009, the owner's settings, 15 s, 3 runs, ms of CPU a second of play):

| rate | main | bindless | kf4-pc main → bindless |
|---|---|---|---|
| `--hz 500` | 494 +-18 | **363 +-7 (-27%)** | 379 → 369 |
| 240 | 200 +-6 | **167 +-4 (-17%)** | 365 → 365 |

Pace (`pace_cut.py`: `pace_test` 25 s at `--hz 500`, plus the gaps between pictures handed to the window, the
pictures between left out as behind and those the window showed late; 3 rounds 15:26-15:55, the order swapped each
round, GPU 2167-2175 MHz at each round's end). Median [min-max] of 3:

| profile | spot | build | pictures/s | game vblanks/s | gap p99 / sd ms | left out | shown late |
|---|---|---|---|---|---|---|---|
| andrew | 000 | main | 355.0 [341.4-358.5] | 60.7 | 14.0 / 2.43 | 471 [369-495] | 2274 |
| | | bindless | **370.1** [361.3-380.7] | 60.6 | 13.8 / 2.23 | 496 [478-500] | 1965 |
| andrew | 009 | main | 331.6 [323.2-334.2] | 60.7 | 13.2 / 2.14 | 613 [574-629] | 2158 |
| | | bindless | 335.4 [329.8-345.2] | 60.6 | 13.9 / 2.16 | 606 [583-629] | 1658 |
| sean | 000 | main | 498.1 [496.6-498.4] | 60.8 | 2.4 / 0.41 | 32 [25-32] | 73 |
| | | bindless | 496.1 [495.3-499.2] | 60.9 | 2.5 / 0.51 | 47 [32-79] | 75 |
| sean | 009 | main | 474.8 [474.0-486.6] | 60.8 | 7.3 / 0.94 | 143 [143-148] | 536 |
| | | bindless | **493.8** [493.7-498.5] | 60.9 | **2.3 / 0.59** | 150 [149-186] | **206** |

Less pump CPU did **not** cost Andrew's uncapped rate this time (the worry in "Before building it" below: #131's push
descriptors lowered it): Andrew's 000 +4%, 009 +1%, the game at 60.6-60.9 vblanks a second in every cell. The owner's
009 now reaches the cap (+4%, the gaps' p99 7.3 → 2.3 ms, shown late 536 → 206); 000 was at the cap already. Left-outs
are the same within the spread. The rate is still bounded by latency, not the pump (the timeline above).
Push descriptors (`KF4_PC_PUSH_DESCRIPTORS=1`) were not measured again: with the table, a kept walk's set 1 does not
change from batch to batch, so pushing it matters only for kf4-pc's own draws.

**What is left** (the design's steps 3-4): records instead of push constants (by `firstInstance`) and runs of
batches with the same pass, pipeline and dynamic state drawn with one `vkCmdDrawIndirect` each. Not started: step 3
alone buys nothing (a push constant is cheap); with step 4 it takes the per-draw driver cost that remains (pipeline,
dynamic state, push constants, `vkCmdDraw`). Step 4 has to build its runs at draw time or check them each picture,
since a batch's target surface, DATE set-up, clears and destination copies are decided in `draw_batch` against the
surfaces as they are.

## Built 2026-11-12 (later): the cut judges on pictures left out, and a frame's pictures between followed

**The cut** (gs.c, `port_gs_run_vsync`): a vblank now counts as behind when the backlog is over 1 as it begins (as
before) **or the vblank before left a picture between out as behind**. Two such vblanks in a row lower the count a
vblank by one, as before (the same settling, raising and doubling rest). `KF4_INTERP_CUT_LEFT=0` judges on the backlog
alone. Left-outs had kept the backlog low (a vblank's pictures left out are not drawn), so the cut saw "caught up" and
the next vblank asked for as many again: pictures came in bursts, the last of each late or left out. Pictures are not
changed where the renderer keeps up: byte for byte perf/vblank-wait's at 000, 009, 009:2, 099 (240,
`KF4_INTERP_DRAW_ALL=1`, 816 pictures). The 16 unit tests pass.

`pace_test` (25 s, `--hz 500`), Windows, RTX 5080, alternated, 3 rounds, **the display off in every round** (owner
at work; GPU 2160 MHz under load), so compare within the table. `tools/pace_test.py`'s numbers plus, from the same
run: the longest and 99th-percentile gap between pictures handed to the window and their spread (sd), the pictures
between left out as behind (`--stats`, whole run) and the pictures the window showed more than half a vblank late
(`--stats` "window"). Median of 3 [min-max]. Before = perf/vblank-wait (b111168a); +#130 = merged with
perf/pc-intake (kf4-pc's intake -30%).

| profile | spot | build | pictures/s | game vblanks/s | gap p99 / sd ms | left out | shown late |
|---|---|---|---|---|---|---|---|
| andrew | 000 | before | 339.7 [336.1-340.4] | 60.7 | 15.1 / 2.31 | 1005 [872-1107] | 3534 |
| | | now | **350.9** [341.8-351.2] | 60.8 | **10.7 / 1.63** | **497** [465-546] | **1592** |
| | | before +#130 | 324.3 [317.6-335.4] | 60.5 | 17.1 / 2.95 | 1336 [1053-1658] | 5084 |
| | | now +#130 | **351.4** [346.5-354.6] | 60.8 | **10.6 / 1.61** | **365** [322-379] | **1607** |
| andrew | 009 | before | 293.0 [288.6-293.1] | 60.5 | 13.9 / 2.14 | 998 [992-1247] | 3068 |
| | | now | 286.5 [284.8-286.9] | 60.7 | 12.1 / 1.94 | 743 [681-745] | 1870 |
| | | before +#130 | 298.0 [295.2-299.0] | 60.7 | 12.6 / 1.92 | 999 [857-1064] | 3275 |
| | | now +#130 | 298.5 [294.2-301.1] | 60.8 | 10.6 / 1.57 | 561 [542-563] | 1976 |
| sean | 000 | before | 378.3 [360.1-392.6] | 60.7 | 13.1 / 2.45 | 71 [53-79] | 3552 |
| | | now | **410.2** [409.5-413.9] | 60.7 | 12.6 / 2.02 | 59 [58-92] | 2540 |
| | | before +#130 | 420.8 [414.9-437.2] | 60.8 | 10.1 / 1.65 | 47 [25-87] | 1821 |
| | | now +#130 | 430.0 [415.5-433.0] | 60.8 | 10.6 / 1.62 | 61 [38-90] | 1502 |
| sean | 009 | before | 382.2 [378.5-385.0] | 60.8 | 11.6 / 1.84 | 763 [630-874] | 3410 |
| | | now | 387.9 [384.6-390.8] | 60.8 | 9.2 / 1.44 | 462 [407-484] | 1981 |
| | | before +#130 | 381.2 [379.5-384.1] | 60.6 | 12.1 / 1.96 | 1335 [1091-1370] | 6815 |
| | | now +#130 | **400.3** [380.2-401.4] | 60.8 | **8.1 / 1.27** | **366** [345-374] | **2324** |

With #110 (perf/pump-draws, the pump's draws 28% less CPU, merged locally), andrew only, same rounds:

| spot | build | pictures/s | game vblanks/s | gap p99 / sd ms | left out | shown late |
|---|---|---|---|---|---|---|
| 000 | before +#110 | 302.3 [296.3-308.9] | 60.0 | 23.6 / 3.65 | 1430 [1297-1621] | 3346 |
| | now +#110 | **342.2** [342.1-344.5] | 60.7 | **12.6 / 2.08** | **495** [456-576] | **1478** |
| 009 | before +#110 | 247.6 [241.0-263.0] | 60.2 | 27.2 / 4.98 | 1997 [1382-2094] | 3949 |
| | now +#110 | **286.7** [286.5-287.8] | 60.5 | **14.3 / 2.26** | **694** [684-765] | **1394** |

At 240 (both profiles, 000 and 009, 2 rounds) nothing moves: 238.9-239.8 pictures a second before and 239.3-239.8
now, 60.7-60.8 game vblanks, the same left-outs (10-35 a run, the load).

The pictures left out as behind fall by a quarter to three quarters and those shown late by about half; the gaps
between pictures are more even (p99 and sd); pictures a second are the same or higher except andrew 009 without #130
(-2%). #130's drop at andrew 000 (340 → 324 with it before) is gone (351 with it now): with the intake faster, the
renderer's backlog fell further and the old cut lowered less while more were left out, which judging on the left-outs
undoes; the same happened with #110 (its pictures sooner, -11% before at 000 and -15% at 009, across separate rounds), and it is gone too.
The longest gap and pace_test's uneven refreshes a second do not move (the longest gap is a hitch, not the
cut; at 500 every refresh whose picture is not new counts as uneven). The cut still lowers only about 28 times a run
(load and settling): it now settles lower and stays (andrew 000, a diagnostic run: allowed 6 a vblank in 1240 of 1497
vblanks, 7 in 257; 18 vblanks left any out).

**The timeline of a frame's pictures between** (a diagnostic build, not committed: `KF4_PICLOG` timestamps at gs.c's
ask, kf4-pc's take, the pump's start and hand-out, kf4-pc's `next_frame_context`, the pump's `start_from`; andrew 000,
500, with the change, 25 s of play, display off). A game frame is 33.4 ms:

| thread | where its time goes (share of play) |
|---|---|
| kf4-pc | the next frame's GIF intake 28% (9.4 ms a frame); `next_frame_context` after each pumped picture 32% (0.87 ms each, p90 2.6: it waits for the pump's open command buffer); idle waiting for the next frame's packets 25% (8.9 ms a frame); the rest 15% |
| pump | the pictures between, drawn back to back 47% (0.9 ms each); `start_from` (the frame's surfaces copied) 12% (3.0 ms a frame, p90 9.2); idle waiting for the next frame 33% (12.7 ms a frame) |
| gs (the vblank thread) | under 1% |

A picture between is asked 26 ms before it is due; it waits 6.8 ms (median, p90 14.7) in kf4-pc's inbox behind the
next frame's packets, then 7.0 ms (p90 13.3) for the pump, which takes it only after the frame's `start_from` and the
pictures before it; it is drawn in 0.9 ms. So it is handed out 11 ms early in the median but 4.6 ms late at p90: the
last pictures of each burst are the ones left out. **What bounds ~6 a vblank is latency, not throughput**: a frame's
pictures between all start after kf4-pc has taken the frame in and the pump has copied its surfaces, and then run
back to back; the pump is idle a third of the time and kf4-pc a quarter, and the GPU (2.1 ms a picture) has room.
Levers, in order: pictures between that do not queue behind the next frame's packets in kf4-pc's inbox (the pump
takes them from gs.c directly once it has the frame's walk); a cheaper or earlier `start_from` (3 ms a frame, 9 at
p90); kf4-pc's `next_frame_context` per pumped picture (32% of kf4-pc; it waits for the pump's command buffer).

## Built 2026-11-12: kf4-pc's intake (#108)

Windows, RTX 5080, on `pc/colour-2d-layerz` ae9a9097 (perf-baseline-1112's base). Profiled first at 009, 240, the
owner's profile (`tools/sampler/profile.py`, 1 ms): kf4-pc 590 ms of CPU a second, `feed` 89%. What it paid for, and
what changed (each its own commit, branch `perf/pc-intake`):

| share of kf4-pc | what | now |
|---|---|---|
| ~14% | native draws' VU1 set-up and 0x280's/0x700's clipping, every operation exact in software (`vu_float_round`, `_add`, `_mul` under `clipped`, `vu_transform4`) | **Plain SSE floats, checked** (`vu1_record.cpp`, as vu1_hle.c's fast way): MXCSR toward zero + FTZ + DAZ is VU1's arithmetic whenever no operand has exponent 255 and nothing overflows, divides by zero, underflows or is invalid; operands checked first, MXCSR's flags after each record or batch, done again exactly otherwise. Built `-frounding-math -ffp-exception-behavior=strict`. `vu1_record_test` compares the two ways on 20,000 troubling records and batches (fails 4,208 checks with the flag check taken out). Out of the profile |
| 6.7% | `keep` hashing every upload to find its chunk | the chunk last kept for the same registers compared first (memcmp); hash four lanes wide for the rest: 2.2% |
| 6.3% | `copied`: each local copy from uploads made again in the scratch VRAM and read back | **kept** by its registers and source chunks (a function of them): 0.5% |
| 6.1% + 2% | byte-at-a-time copies (libc++'s range insert/construct of `unsigned char`) of uploads gathered from packets, chunks, the frame's kept uploads | memcpy; a found upload's kept copy is its chunk's bytes (shared) |
| ~3% + 3% | `clut_colours` made for every batch through the CLUT (twice); `set_mirror` marking the CLUT page 512 times a load | the last answer kept while the mirror and TEX0/TEXA's fields are the same; the page marked once a load |

Checks: pictures incl. every picture between (`KF4_INTERP_DRAW_ALL=1`) at 000, 009, 009:2, 099 at 240, 4x (OWNER) and
1x, byte for byte the base build's (1,632); all 16 `*_test.exe` pass.

**A/B**, display on (nvidia-smi `display_active` Enabled, 2865-2880 MHz), alternated, 3 rounds. CPU (`build_cost.py`, 15
s, 009, the owner's profile; base = `hl-base` ae9a9097), ms of CPU a second of play:

| thread | 240: base → now | 500: base → now |
|---|---|---|
| kf4-pc | 505 (504-507) → **375** (366-380), -26% | 510 (486-540) → **387** (380-400), -24% |
| kf4-pc-pump | 296 → 284 | 732 → 714 |
| kf4-dma, kf4, kf4-between | 203, 191, 89 → 200, 189, 86 | 202, 190, 98 → 201, 193, 102 |

Each frame's intake (`--stats`' new "pc intake a frame", the base with only that commit; from the pace runs below,
whole runs, medians of 3): **busy**, kf4-pc's time taking a frame's packets in, and **ready**, from its first packet
to its pictures between handed their frame. Median (p95), ms:

| profile | rate | spot | busy | ready | pictures a second | lowest second | game vblanks a second |
|---|---|---|---|---|---|---|---|
| andrew | 240 | 000 | 9.38 (10.50) → **6.89** (7.80) | 11.34 (18.34) → 8.81 (17.32) | 239.2 → 239.8 | 233 → 239 | 60.8 → 60.8 |
| andrew | 240 | 009 | 12.23 (14.46) → **8.57** (9.93) | 14.85 (18.24) → 11.15 (17.15) | 239.7 → 239.8 | 238 → 239 | 60.8 → 60.8 |
| andrew | 500 | 000 | 9.12 (11.30) → **6.47** (8.53) | 10.90 (15.33) → 8.42 (13.02) | 362.3 [349.5-362.5] → 352.7 [350.9-354.3] | 326 → 333 | 57.3 → 56.5 |
| andrew | 500 | 009 | 12.33 (15.44) → **8.14** (11.02) | 14.17 (17.83) → 10.52 (14.22) | 337.0 [334.3-342.6] → **346.7** [346.0-348.7] | 293 → 330 | 59.0 → 55.1 |
| sean | 240 | 000 | 9.13 (10.17) → **6.56** (7.49) | 10.97 (18.51) → 8.22 (17.44) | 239.8 → 239.8 | 239 → 239 | 60.6 → 60.8 |
| sean | 240 | 009 | 12.59 (14.17) → **8.44** (9.64) | 15.01 (18.25) → 10.61 (17.42) | 239.5 → 239.5 | 235 → 237 | 60.8 → 60.7 |
| sean | 500 | 000 | 9.11 (10.32) → **6.48** (7.51) | 10.60 (17.81) → 8.13 (17.11) | 498.1 → 499.2 | 484 → 480 | 60.8 → 60.8 |
| sean | 500 | 009 | 11.90 (14.73) → **7.89** (10.23) | 14.08 (18.10) → 10.42 (17.30) | 472.5 [467.5-474.5] → **493.7** [492.4-495.5] | 428 → 428 | 60.6 → 60.6 |

A frame's intake is 28-34% shorter, its pictures between ready 2.5-3.7 ms sooner (the p95 of ready, ~17 ms, is
frames that wait behind a picture kf4-pc makes itself, not intake). Uncapped, the owner's 009 now nearly reaches the
500 cap (+21 a second) and Andrew's 009 gains ~10 a second; Andrew's 000 is within its noise. Andrew's game vblanks
a second at 500 fall (009: 59.0 → 55.1): that is #107's vblanks left off the screen, not the game. With the pictures
coming sooner the cut lowers the count a vblank less often (009: 2,488 → 1,569 vblanks drawn with fewer), more
pictures are asked than made, and more are left out as behind (861 → 2,074), whole vblanks among them; #107's fix
(b111168a, keep one picture between for a vblank behind) is what stops that. The same at 500 with b111168a's gs.c applied to both builds (3 rounds, display on):

| profile | spot | pictures a second | lowest second | game vblanks a second | busy | ready |
|---|---|---|---|---|---|---|
| andrew | 000 | 387.3 [360.0-389.8] → 355.0 [354.2-364.2] | 331 → 334 | 60.6 → 60.4 | 9.13 → 7.25 | 10.83 → 9.00 |
| andrew | 009 | 337.6 [333.5-338.2] → **351.8** [350.0-353.2] | 294 → 338 | 60.6 → 60.8 | 12.34 → 8.73 | 13.88 → 10.69 |
| sean | 000 | 498.9 → 499.0 (the cap) | 478 → 483 | 60.8 → 60.8 | 9.10 → 5.96 | 10.67 → 8.21 |
| sean | 009 | 478.7 [413.6-479.3] → **491.3** [489.5-497.3] | 429 → 459 | 60.8 → 60.7 | 11.92 → 8.55 | 14.22 → 11.09 |

The game keeps 60 vblanks a second then. Andrew's 000 shows fewer pictures in pace_test's 25 s though the renderer
draws more over the run (23,457-23,892 pictures between against 22,364-22,679): the cut, which judges only on the
backlog, now seldom lowers the count a vblank (151-422 vblanks drawn with fewer, against 1,127-1,550), so more of the
8-9 asked are left out as behind (1,179-1,275 against 774-1,077). That is item 1's "the cut judging on pictures left
out" again; intake is no longer what holds those pictures back.

## Built 2026-11-12: the pump's descriptor sets and submits (issue #110)

**Profiled first** (`ae9a9097`, Windows, RTX 5080, 009 uncapped `--hz 500`, the owner's settings, `build_cost.py` +
`stacks.py`, display on): the pump 709 ms of CPU a second, `draw_again_alone` 95% of it; the NVIDIA driver 66% self;
`flush_render_state` 59% inclusive, of it `flush_descriptor_sets` 39% (`flush_descriptor_set`, a new set allocated
and written with `vkUpdateDescriptorSetWithTemplate`, 32%); submits 5.5%; dynamic state 6%; pipeline binds 4%.
Why so many sets: three things, each found in Granite's code, not guessed:
- Granite's `set_uniform_buffer` / `set_storage_buffer` (the path without descriptor buffers, which pcr uses) marked
  the set for a new descriptor set even when the same buffer was bound again (set_texture and the descriptor buffer
  paths return early). The canvas binds the pictures between's three buffers, a native batch's three and the CLUT's
  empty buffer at every batch: up to three new sets a batch.
- The batch programs' sets: textures (changing at most batches) in set 0, the pictures between's buffers in set 1 laid
  out differently for GIF and native programs (so every switch between them reallocated sets 1 and 2), the native
  buffers in set 2.
- Push descriptors were never on: Granite pushes the last set of a layout only when the context is made with
  `CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT`, and pcr passed no flags.

**Built** (branch `perf/pump-draws`; pictures byte for byte the base build's):
1. `cmake/granite_descriptor_reuse.cmake`: a buffer bound again unchanged keeps its set (Granite patched at configure,
   as the other granite_*.cmake).
2. **One `vkQueueSubmit` a picture between** where there were two: the canvas's command buffer is handed to Granite
   without a flush where a flushing submit follows at once (in `scanout`, always; at the end of `draw_again_alone`
   when the picture that shows it is already the pump's next message: uncapped rates. At a capped rate it is flushed at
   once as before, so the GPU draws it while the picture's time comes).
3. **One descriptor layout for every batch program** (`make_programs`): set 0 the vertex stage's buffers, set 1 the
   fragment stage's textures and CLUT, the last set.
4. **Each native program's buffers in bindings of their own** (set 0: 3-5 native 000, 6-8 280, 9-11 700, 12-14 the
   ripple's): a frame's batches of one program share their buffers, so set 0 is written again only at a render pass
   (Granite forgets bindings there: `set0_fresh_`) or when a frame's buffers grow.
5. **Push descriptors, but off by default** (`make_context`, `KF4_PC_PUSH_DESCRIPTORS=1`): the textures' set pushed.
   `KF4_PC_SUBMIT_TOGETHER=0` undoes 2, to compare.

With push descriptors (profile of the new build, same settings): the pump 443 ms a second; `flush_descriptor_sets`
13% of it (12.4% the push itself, no set allocated or written a batch); the driver 61% self; submits 6% (now mostly the
every-128-batches flushes); dynamic state 9%, pipeline binds 8%. Without them (the default), 488-518 (one run; the
A/B below): set 1 is allocated and written at each change of a batch's textures, set 0 only at a render pass.

**Why push descriptors are off.** Alternated at 009 `--hz 500` (pace_test, 2 rounds, display on): Andrew's settings
296 pictures a second with them [285-307], 321 without [316-327], the game 56.5 against 58.4 vblanks a second; the
owner's 494 against 491. Less CPU in the pump and a lower uncapped rate: the rate there is not bounded by the pump
(#107: the threads' latency). Why a quicker pump costs Andrew's settings pictures (and, with push descriptors, the
game vblanks) is not found; candidates, not checked: Granite's device lock (the pump's submits and kf4-pc's frame
contexts), the order the pump's and kf4-pc's pictures reach the presenter's ring. `KF4_PC_SUBMIT_TOGETHER=0` did not
change it (one run: 294 against 306, push descriptors on). Until it is found, the default keeps the rate. The same shape, smaller, shows between the base and the new build without them (below).

**A/B** (base `ae9a9097` against this branch, push descriptors off as by default; 009, Windows, RTX 5080, builds
alternated, display on unless said: `display_active` Enabled and 2880 MHz at each round's start and end):

| | base | new |
|---|---|---|
| pump CPU ms a second, `--hz 500`, owner's settings (`build_cost.py`, 3 runs) | 719 +-9 | **518 +-5 (-28%)** |
| kf4-pc / kf4-dma / kf4 (same runs) | 505 / 205 / 190 | 483 / 203 / 192 |
| pump CPU ms a second, 240, owner's (3 runs, **display off**, noisy) | 326 +-38 | 251 +-22 (-23%) |
| pictures a second `--hz 500`, Andrew's (pace_test, rounds 1-2) | 334.1, 334.2 | 320.0, 312.8 (**-5%**) |
| game vblanks a second, Andrew's | 58.5, 59.0 | 58.4, 58.5 |
| pictures a second `--hz 500`, owner's (rounds 1-2) | 481.3, 396.2 (lowest second 236) | 486.9, 483.5 |
| lowest second / uneven refreshes a second, owner's | 426, 236 / 43, 214 | 420, 469 / 41, 44 |
| GPU ms a picture (both profiles) | 1.81 / 1.40 | 1.81-1.84 / 1.40-1.41 |

Round 3 is left out: the display slept during it (08:30; both builds fell alike, e.g. Andrew's 274 / 271). With push
descriptors on, 3 rounds all display on (07:28-07:41): Andrew's 333.7 [333.1-343.7] against 290.3 [286.4-296.4],
the owner's 481.4 [480.9-482.6] against 494.8 [492.8-495.1] (lowest second 422 against 482), the pump 728 +-34 against
437 +-11. So the owner's uncapped rate is the same or a little better and steadier, and **Andrew's is about 5% lower
without push descriptors and 13% lower with them, while the pump does less**: not explained yet (above). At 240 every
spot held the cap in both builds before; not measured again.

**Checks.** Pictures byte for byte the base build's at 000, 009, 009:2, 099 at 240 with every picture between
(`KF4_INTERP_DRAW_ALL=1`, vblanks 2400-2403 and 2460, 20 pictures a spot), 1x and 4x MSAA 2, with push descriptors
off (the default) and on. The validation layer with sync validation at the four spots (1x, 4x MSAA 2; with push
descriptors at 000 and 009): only VUID-RuntimeSpirv-OpEntryPoint-08743 at 009, the base build's (pcr_ndripple.vert's
missing `v_rect`, fixed on `perf/gpu-picture`). Unit tests 16/16 (Windows). Not run: Linux/RADV, `KF4_PC_PUMP=0`,
`--renderer gs` (its Granite gets the same patch, which only skips identical rebinds).

### Designed: bindless textures and merged draws on the kept walks (steps 1-2 built 2026-11-12, above; 3-4 not)

**What a kept walk is to the GPU.** The pump draws the same walk (`Kept`: its batches, uploads, copies, marks) for every
picture between of a game frame: three times a frame at 240, seven at 500. Only set 0's picture buffer (the time and
camera) differs between those pictures; every batch's textures, CLUT, vertices, push constants, pipeline and dynamic
state are the same each time. Today each picture still records every batch as the frame did: set its textures, push
its constants, set its dynamic state, one `vkCmdDraw` (a native batch one per run). The work a picture can be moved to
the walk, once a frame.

**1. One descriptor table a walk (bindless, `VK_EXT_descriptor_indexing`, core in 1.2).**
- When a walk is handed out (`take_kept_walk`, `as_picture_walk`, where `put_vertices_on_gpu` already runs), every
  image view its batches read that lives as long as the walk (`BoundTexture::held`: decoded textures and sampled
  ones; the dummies) gets an index in one array descriptor set, written once (Granite: `BindlessAllocator::push`,
  `commit`, bound with `CommandBuffer::set_bindless`). Each batch keeps its indices: texels, sampled.
- Granite allows: a bindless set holds **one binding, an unsized array of sampled images** (`texture2D[]`, no
  sampler: `layout.bindless_set_mask`, "Can only use bindless for sampled images"), with update-after-bind pools.
  So samplers go elsewhere: the GS's filter and wrap choices are few (`BoundTexture::sampler` comes from a small
  stock), so a fixed array of `sampler`s in an ordinary set, indexed from the push constants. The integer texels
  (`usampler2D u_texels`, R8_UINT / R8G8B8A8_UINT) alias the same binding as `utexture2D[]` (a second declaration at
  the same set and binding: legal, the descriptor is a sampled image either way).
- Indexing needs `nonuniformEXT` only if one draw reads several textures; per draw the index is uniform (push
  constant or flat), so plain dynamically uniform indexing (`shaderSampledImageArrayDynamicIndexing`) is enough.
- **CLUTs**: a walk's CLUTs (1 KB each, today `allocate_constant_data` at every CLUT batch of every picture) go into
  one buffer of the walk's when it is handed out, as its vertices do; a batch keeps its offset (push constant), the
  shader reads `clut[offset + index]` from a storage buffer bound once in set 0.
- **What stays per draw**: a texture read from a surface (`surface_copy`: copied from a surface at the draw, in the
  pump too: the kept frame's reads between 0xF4 and 0xF5) and the destination copies of shader blends and DATE by a
  copy (`dest_copy`, made at the draw). They are made after the table is written; they keep today's set 1 (pushed or
  allocated), a handful a picture at the measured spots (`textures_from_surfaces`, `unsupported_blends`).
- On the GPU: the table is per walk, so a walk's images are held by the walk (they are now) and the table freed with
  it. Update-after-bind is not needed: a table is never changed once a picture records with it.

**2. A walk's batches as records, drawn with few calls.**
- Today's `PrimPush` (24 words) becomes a per-batch record in a buffer of the walk's (written once when handed out),
  plus the texture/sampler/CLUT indices; the shader finds its record by `gl_InstanceIndex` / `gl_BaseInstance` for GIF
  batches (drawn with `instanceCount` 1, `firstInstance` = the record), and for native batches by a word added to
  the instance's record (they already use `firstInstance` for `draws[]`). The push constants keep only what a picture
  changes (nothing at present) and the pass (`F_PASS_SHIFT`).
- Runs: consecutive batches into the same render pass with the same pipeline (program, per-sample or not) and the same
  dynamic state (blend, depth test/compare/write, colour mask, stencil, blend constants, scissor) form a run. A run is
  one `vkCmdDrawIndirect` over a command list made once per walk (`draw_count` = the run's draws, one per batch or
  native run); Vulkan keeps primitive order across the draws of one indirect call, so the GS's order (blending, Z
  ties) is kept. Granite has `draw_indirect(buffer, offset, count, stride)`; it calls `flush_render_state` once.
- Breaks a run: a render pass change (another target surface), DATE's stencil set-up, a shader blend (its
  destination copy), a pass whose state differs, an upload or copy into a surface the walk draws on, a batch with a
  per-draw texture (above). Scissor could be folded into the record (a `discard` outside it, or clipped in the vertex
  shader), but costs pixels; first measure how often it alone breaks runs.
- Multi-pass batches (alpha-test passes, `pass_count` 2-3): each pass is a different dynamic state; the run is per pass
  state, so a batch's passes stay consecutive draws, or the walk's batches are regrouped by pass only where they do
  not overlap (not attempted: order matters).

**What it should buy.** The pump's per-draw work today (after the commits above): Granite's `flush_render_state` and
the NVIDIA driver's per-draw cost (set 1, push constants, dynamic state, `vkCmdDraw`). With (1) alone a picture
writes no descriptors at all (one table bind a picture); with (2) the calls go from one per batch (~N hundred at 009) to
one per run. kf4-pc's own frame draws (the walk being made) are not helped: they are drawn once, and their textures
are made as they go.

**Order to build it, each step checked byte for byte at the four spots as today:**
1. CLUTs into the walk's buffer (smallest; a storage buffer in set 0; no bindless yet).
2. The bindless table for held textures + a sampler array; per-draw textures stay on set 1 (a flag picks the source).
3. Records instead of push constants for kept batches.
4. Runs as indirect draws; `KF4_PC_RUNS=0` keeps one call a batch to compare.
Granite needs nothing patched for 1-3 (its bindless and indirect paths exist; the device must be made with
`descriptorIndexing`, which Granite enables when present). Not considered: Granite's descriptor buffer / descriptor
heap paths (`CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT`): they change every Granite user in the process
(paraLLEl-GS too) and are newer in drivers.

**Before building it**, find why less pump CPU lowered Andrew's uncapped rate (push descriptors, above): merged draws
cut the pump's CPU much further, and if the pump finishing sooner costs the game vblanks, that has to be understood
first (the 2026-11-06 per-picture timeline at 500, Granite's device lock held by the pump's submits).

## The list, best first

0. **Done 2026-11-07**: item 1's reference counts and pipeline changes (dynamic state, a walk's vertices on the GPU
   once), above: the pump 13% less CPU, the uncapped rate about the same.
   **Done 2026-11-07**: pages marked again (item 1) and kept steps out of the variant (item 2), above.
   **Done 2026-11-06**: the pictures between of every second vblank, and the pump's pictures on the GPU's queue when
   handed out. **Done 2026-11-07**: the in-game profiler (docs/port/PROFILING.md), which the items below come from.
   The pump (the old item 1) is done too (305b216a).

1. **What limits the uncapped rate**: the game's 57-58 vblanks a second are **fixed 2026-11-12** (above: vblanks left
   off the screen, not the game slowed; 60.4-60.8 now). Left: the rate of pictures between itself (~6 a vblank made
   of 8-9 asked at 500, neither the pump nor the GPU full). **The cut judges on pictures left out (2026-11-12,
   above)**: half the left-outs and late pictures, more even gaps, the same or more pictures a second, #130's drop
   at 000 undone. **The timeline is followed (above)**: the bound is latency: a frame's pictures between wait in
   kf4-pc's inbox behind the next frame's packets and for the pump's `start_from`, then run back to back; next, the
   pictures between to the pump without kf4-pc's queue, and `start_from` cheaper.
   **The pump's cost a batch** (less than measured above: the pump is 9.3 s of CPU in 20 s at 240 now):
   - **One submit a picture between**: **done 2026-11-12** where the picture that shows it is already waiting
     (uncapped), below. The pump's other submits (every 128 batches) are left.
   - **Descriptor sets: done 2026-11-12** (#110, above: the pump 28% less CPU at 009 uncapped; Andrew's uncapped rate
     5% lower, not yet explained). Left of the old item 2: **merged draws and bindless textures** for the per-draw
     cost that remains (the driver's draw, set 1 and dynamic state, about 60% of the pump): designed above. **Bindless
     textures built 2026-11-12** (steps 1-2, above: no descriptor set a batch in a kept walk; the pump -27% CPU at 009
     uncapped); merged draws (steps 3-4) not built.
2. **`kf4-pc`'s intake**: **done 2026-11-12** (#108, above: the VU maths, uploads found without hashing, copies
   and CLUT colours kept; kf4-pc -24 to -26% CPU at 009, a frame's intake -28 to -34%). Kept steps out of the variant were done 2026-11-07. Left of it (009, 240):
   - **An upload owned again block by block** (`own` → `take_nibbles`, ~11%): mostly cache misses on each block's
     owner list, even for an upload found again whose blocks it already owns newest. A chunk marked as owning all
     its blocks newest (cleared by any other upload or draw over them) would let `own` skip the walk.
   - **The bytes themselves** (~5% `memcpy` of uploads gathered from several packets, ~4% `memcmp` finding them):
     the price of reading what the game sends; only fewer copies (an upload kept where its packet is) would help.
   - **`Batch` copies** (`take_kept_batch`, `in_picture`: ~5%) and texture binding per batch (`bind_texture`
     ~12%: `resolve`'s kept answers, `sampled_texture`'s hash of 256 CLUT colours).
   - The NVIDIA driver (~17% self: kf4-pc's own draws and uploads) and Granite's descriptor sets (~6%).
3. **The GPU.** **Done 2026-11-12 (#109, PC_RENDERER.md "GPU time a picture")**: native 280's colour carry no longer
   walks the strip at every corner, draws that discard nothing are depth tested before their shader, a pass after a
   pass waits only for its attachments, the game frame's clears are cleared. GPU a picture at 240 (Windows, RTX 5080,
   display on), base -> new: Andrew's profile 1.52/1.68/1.13/1.27 -> 1.37/1.47/1.01/1.17 ms at 000/009/009:2/099
   (-8 to -12%), the owner's 1.23/1.33/0.90/0.97 -> 1.12/1.17/0.84/0.92 (-6 to -12%); pictures byte for byte. MSAA 2
   costs about 0.19 ms at 009 (the owner's 3x), colour high about 0.21 more. Left: native 000 (and the ripple) is the
   biggest kind now; the native vertex shaders redo each vertex for every triangle (a per-vertex pass first); the kept
   frame's MSAA resolve (0xF2) of the whole surface.
4. **`fast` switched to `pc` (step 7)**, with the settings pc lacks (MSAA 2, `--native-colour high` and the
   others): measured 2026-11-12 (item 3): MSAA 2 about 0.19 ms a picture at 009, colour high about 0.21 more.
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
   - Windows and NVIDIA (RTX 5080, 2026-11-11, PC_RENDERER.md "Windows, NVIDIA and the Vulkan validation layer"):
     run and checked for pictures, not measured for pace (a game was running beside it on that machine: every
     rate it gave is unreliable). Still to do there, on an idle machine: `pace_test.py --maps 0,9,9:2,63` at 240
     and `--hz 500`, alternated, 3+ runs. Not run: Intel, other AMD drivers.

Measure each with `pace_test.py` (uncapped, `--hz 500`, alternated, 3+ runs: runs differ by about 5 a second) and
the profiler's share of the thread it targets.

# Smooth play: the plan (proposed, October 2026)

The owner's report: on a top GPU (RX 9070 XT, 170 Hz screen) the game still drops pictures, stalls, and logs
`a pipeline not on the list was made in play`; it should be much better than it is, and have an unlocked frame
rate. This plan takes each cause measured below to its fix, in order of what the owner feels, and adds
`--frame-rate unlocked`.

Status: agreed in general by the owner (2026-10-28); step 1.4 and the targets are still to be decided. Built: the
pipeline set (step 1.2, below), the files written on a thread of their own (1.3) and one count, not a line a
pipeline (1.5). The refresh clock (port/platform/refresh_clock.c, screen_vblank.c) is step 0, on its own branch
(`smooth/refresh-clock`). Step 2 measured again and split (2a, the recording first; agreed 2026-10-28), on
`smooth/picture-pump`; 2a as first written checked and found not to work (below), and instead the recording made
cheaper: a frame recorded once (built).

## What was measured (main 74149928 + the refresh clock, 2026-10-28)

Headless, `KF4_PRESENT_SIM=169.83` (the owner's screen), the owner's settings from `settings.json`: `--renderer
fast --native-draw on --native-scale 4 --native-widescreen --native-msaa 2 --native-aniso 16 --native-cutout-aa
ssaa --native-colour high`, newgame.pad and `--teleport 1900:MAP:0 --invincible`, 30 s of play, a fresh
`KF4_DATA_DIR`. Ryzen 7 5700X, RADV (Mesa 26.2). A real window was not measured (below, step 0).

| run | pictures a second (lowest second) | refreshes not one step of the world | longest gap in play |
|---|---|---|---|
| 000, `--frame-rate monitor` | 169.1 (165) | 0.9% | 23.6 ms |
| 009, `--frame-rate monitor` | 169.8 (168) | 0.1% | 11.8 ms |
| 000, `--frame-rate 240` (the owner's setting) | 221.8 (216), 654 pictures left out | **9.4%** | 16.9 ms |
| 000, monitor, driver's shader cache off | 169.2 (164) | 0.5% | compiles: **7.7 s in all, the longest 1,058 ms** |

What each says:

1. **The pipelines.** The shipped list (`gsn_pipeline_list.h`) was last made on 2026-10-24, before `fast` existed,
   and has no line for `--native-colour high` (tag `k1`). For the owner's settings (`m2.c1.k1.s1.x0`) nothing is
   made ahead: `pipelines made ahead: 224 on the lists, 0 built`. Every pipeline `fast` needs is made by the draw
   that first needs it, with the GS's thread waiting. On RADV with Mesa's own disk cache warm that is 0-2 ms each
   (25 on map 000); with it cold (a new Mesa, a shader changed by a new build, a first run) it is seconds in all,
   and up to a second for one. Every build that changes a shader makes its lines in both lists stale, so the owner,
   who builds often, meets them again and again.
2. **The pipeline cache is written on the GS's thread** (`save_pipeline_cache`, gsn.cpp): 3.5 MB from
   `vkGetPipelineCacheData` and a file written, two seconds after any pipeline is made. A second, smaller hitch
   after each compile.
3. **240 on a 169.83 Hz screen judders by design.** Pictures timed for 240 a second land on 170 refreshes: one in
   ten refreshes shows the world move by two steps or none, and the GPU draws 70 pictures a second nobody sees. The
   owner's `settings.json` has `frame_rate: 240` with vsync off.
4. **The GS's thread works in bursts, and is nearly full.** The frame's packets (gif ~13 ms) and the back end's
   flush come within one vblank: about 20 ms (p99 26, worst 45 ms on 000). That is not all a game frame costs it:
   the recording of the pictures between comes in the next vblank (step 2, measured again). Over 33 ms the
   pictures between are cut (`cut to fewer a vblank`). RENDERER.md, "Where 240 stands", found the same at 009 at
   240.
5. **The refresh clock's latency is taken from the hitches.** The time before each refresh a picture is made
   for is the 98th percentile of the last 256 pictures' lateness: 6-7.5 ms after a clean run, 17 ms after the
   cold one. A compile makes every picture after it a refresh later than it needs to be.
6. **The GPU's time a picture is far above what the scene needs.** 3.46 ms a picture at 4x MSAA 2x
   (`KF4_GSN_GPU_TIME=2`, 000) for about 21,000 triangles. A picture is 13.6 render passes and 11.6 resolves,
   with 221 MB loaded, 233 MB stored, 204 MB resolved and 78 MB copied for blending: about 0.7 GB of memory
   traffic a picture. 156,137 of 193,234 render passes ended for shader blending. By kind: scene draws 0.54 ms,
   passes untagged 0.55, resolves 0.49, skinned groups 0.39, Z-first alpha-tested 0.38, MSAA loads 0.28, scene
   ops 0.26, GS batches 0.20. This is the ceiling of any unlocked frame rate: about 285 a second today.
7. **The game's thread hashes about 5 MB of meshes a game frame** (native draw finds its meshes by content: 5.9 GB
   hashed in the run). It keeps up on this CPU (no game vblank over 25 ms in play) but it is the first thing a
   slower CPU or a busier map loses.

## The targets

At the owner's settings, on this machine and on Windows with an NVIDIA card (the work is platform-neutral):

- **No stall from a pipeline, ever**: no gap over two refreshes from a compile in play, including a first run of
  a new build on a cold driver cache.
- **Steady play**: at least 99.5% of refreshes one step of the world, at 170 and 240 Hz, every spot of the map
  tour; no second under the refresh rate.
- **GPU at most 1.5 ms a picture** at 4x MSAA 2x (from 3.46), so that unlocked reaches 500 a second and 240 Hz
  keeps up with room.
- **`--frame-rate unlocked`**, as smooth as `monitor` where the screen can show it (VRR, or tearing allowed).

## Step 0: land what is in flight, and measure what the owner sees

- Commit the refresh clock (`refresh_clock.c`, `screen_vblank.c`, `refresh_clock_test`) with its checks.
- **A real window's record.** `KF4_PRESENT_LOG` in a real window, with each picture's present and, where the
  system knows it, the refresh it reached; `pace_test.py --window` reads it. The headless runs above cannot see the
  compositor, the swapchain or fullscreen; this is how the owner's own play gets numbers.
- **`pace_test.py --cold`**: a fresh data folder and the driver's cache off (`MESA_SHADER_CACHE_DISABLE=true`;
  `__GL_SHADER_DISK_CACHE=0` on NVIDIA); prints the compiles made in play, their total and the longest. Step 1's
  check.
- Owner, now: set the frame rate to **monitor** (the Debug page, or `"frame_rate": "monitor"`): it is item 3
  above, and it costs nothing.

## Step 1: pipelines never stall play

Why there is building at all: the shaders are compiled ahead, into SPIR-V (`gsn_shaders.h`), but SPIR-V is not
what a GPU runs. The driver turns each pipeline (shaders together with the render pass's formats and samples, the
vertex layout and the state not made dynamic) into its own GPU's machine code, which differs by GPU and by driver
version; a pipeline cache from another GPU or driver is refused by its header. So the machine code cannot be
shipped (Steam's shader pre-caching does it by collecting caches per GPU and driver from players). What can be
done is to make every pipeline before play and keep the driver's result on disk: this step makes that complete.

In order; each is useful alone.

1. **The shipped list made again, for `fast` and every tag the settings offer** (MSAA 1/2/4/8, colour exact and
   high, cut-out AA, the scene's samples), from map tours with `--renderer fast` and `modern`. A ctest checks the
   shipped list against the built shaders: no stale line, every tag the settings page can produce present. A
   shader change that forgets to remake the list then fails the tests, instead of the owner's next play.
2. **Lines not tied to the settings.** A pipeline's shaders and state are the same at MSAA 2 and 4; only its
   render pass differs. The warm-up makes each line again with the render pass of the settings in use
   (attachments' formats and samples rewritten, then checked by Granite's hash as now). One tour then covers every
   combination, and the data folder's lines made at other settings count too.
   **Then the whole set worked out, not found by playing: the pipeline set** (agreed 2026-10-28; built, below).
3. **The pipeline cache and the data folder's list written on a thread of their own**, not the GS's. **Built**:
   the GS's thread only says the files are wanted; `kf4-gs-files` takes the cache's bytes from Granite (about 3 ms
   at 3 MB, 1.2 ms more of it allocating) and writes them (1.3-1.5 ms). Taking the bytes was the larger part, and
   was not safe on any thread: on RADV Granite keeps pipeline binaries (`VK_KHR_pipeline_binary`) and walked their
   maps without their locks, while the back end's thread and the warm-up's could be adding to them. A Granite edit
   made at configure (`cmake/granite_pipeline_warm.cmake`) holds the maps' read locks for the walk. The cache is now
   written while a cold warm-up runs too (every two seconds, as it grows), so a start cut short keeps what was
   built. Checked: TSan over a cold start with the files written seven times while two warm-up threads compiled,
   nothing from this code or Granite's cache; the next start took all 184 pipelines from the file.
4. **A draw in `fast` never waits for a compile.** A pipeline not made is handed to the warm-up's threads, and the
   batch is not drawn until it is (a few pictures with RADV's cache warm; up to a second cold). `modern` and
   `native` are the references and still wait (their pictures are checked byte for byte). **Owner's choice**: an
   object missing for a few pictures, or a stall; this plan proposes the first for `fast` only.
5. **One line, not one a pipeline**: `--stats` and Shift+P's profiler count the compiles made in play and their
   time; the log keeps a line only for those over 5 ms. **Built**: `port_profile_compile` counts them whether the
   panel is open or not (`profile_test`); the profiler's line and `--stats`' `pipelines made ahead` give the count,
   the time in all and the longest.

Check: `pace_test.py --cold` at 000, 009, 063 and 022: no picture gap over two refreshes.

### The pipeline set (`gsn_pipeline_set.inc`)

The start already built pipelines ahead ("Building shaders... N/M"); what it lacked was a complete list, since the
lists are what tours happened to draw, at the settings they ran with. The set is every pipeline the draws can ask
for, worked out from the code for the settings in use.

**Why it is small.** Granite keys a pipeline on its program (vertex and fragment shader), the compatible render
pass (the attachments' formats, the subpass), the vertex layout and the state it bakes in. With dynamic state
(`dynamic_state_choice`) depth, stencil, culling, blending and the write mask are set at each draw and left out of
the key. Decoded, every pipeline in the recorded lists had the same baked state (a triangle list, the dynamic bits)
and one of two vertex layouts. So a pipeline is a program and a pass shape: at the owner's settings the lists held
33 graphics pipelines from 25 programs and 9 pass shapes, and 14-57 at other settings.

**How it works.**
- Each place that draws is an entry of `ps_sites[]`, a function that adds the pipelines it can ask for:
  the program and the pass it draws in (`PsPass`: colour format, depth format, samples, subpass: none,
  `msaa_subpass`'s, `scratch_subpass`'s or `resolve_subpass`'s, the last split out of `begin_pass` for this), the vertex layout (none, `Vtx`, native draw's `Corner`) and
  conservative rasterization; or a compute program. A new kind of draw is a new entry.
- The passes are made from the same functions the draws take them from: `colour_format_of`,
  `sa_depth_format_wanted`, `sa_samples_for` (split out of `sa_switch` and `sa_shape` for this), at the settings
  wanted, since the targets take them only at a vblank after the start. The samples matter more than they look:
  Granite's hash of a compatible render pass leaves them out, so a pipeline made at the wrong samples would be found
  by a draw at the right ones.
- At the start (`pw_start`, from `gsn_pipeline_cache`) the entries are listed, each pipeline's `DeferredPipelineCompile`
  made as a `Cmd`'s draw would have it (Granite's opaque state with the dynamic bits) and its hash worked out with
  Granite's own function, and those not already on a list are added to the warm-up's jobs, made on its threads
  before play. The entries are made in the order the game needs them: the scanout, the copies and the GS's 2D (the
  title) first, the world last.
- **The check:** a pipeline a draw makes in play is counted against the set (`--stats`, "pipeline set"), and with
  `KF4_GSN_WARM=set` (the set alone made ahead, not the lists) each one outside it is logged with its shaders'
  names (`gsn_spv_all` now has them, from `tools/build_shaders.py`) and its pass. Outside the set means an entry is
  missing or names a pass wrong. The set's hashes are also checked against the lists': at the owner's settings all
  35 recorded (33 graphics, 2 compute) are in the set.
- Not covered: a device without blending as dynamic state (no `VK_EXT_extended_dynamic_state3`): the blend
  equation is then in the key, and the lists alone are used, as before. And the upscalers' composite and motion
  passes (`gsn_composite.inc`), `--upscaler` only: not entries yet.

**The entries** (184 pipelines at the owner's settings):

| entry | what | pipelines |
|---|---|---|
| the scanout | `narrow_pass`, the movies' fields, the picture scaled and sharpened: 8-bit, one sample | 4 |
| the full-screen copies | a target's picture and Z into its multisampled images and back, the copy shader blending reads in the pass; the scene's taken from the targets, given back, resampled | 24 |
| the GS's batches | in a target's pass: Z clamped with its Z buffer, the plain program without; Z only; cut-outs at every sample; shader blending from the scratch; multisampled, resolving the copy the next run blends from; native draw's screen passes and trail (2D) over the plain programs; without the scene, the models' own vertex programs (`nd_program_of`: made whether native draw is on yet or not) | 40 |
| the replay | the pictures between's compute programs | 2 |
| the GS's draws in the scene | `sa_draw_range`'s PS2 corners and the water, in the scene's pass | 36 |
| the scene's draws | its batches, skinned groups, the PC world's materials, the ties' Z-first draws, shader blending in the pass and its copies, in the scene's pass at its samples and at one, with the scratch or not | 78 |

**Measured** (map 000, the owner's settings, 30 s of play, a fresh data folder each run):

| | before (the lists) | the set |
|---|---|---|
| driver's cache warm: pipelines made in play | 25 | **0** (all from the cache in 0.01 s) |
| driver's cache cold (`MESA_SHADER_CACHE_DISABLE=true`): made in play | 25 compiles, 7.7 s, the longest 1,058 ms | **0** in play (1 by the title before play, 3.9 s) |
| cold: made ahead | 0 | 177 in 36 s on 8 threads; play waited 13.7 s |
| cold: refreshes not one step, the longest gap | 0.5%, a 4-refresh jump 6 times | 0.7%, 25.6 ms (as with the cache warm) |

(The cold rows are of the set at 177, before the resolve passes' 7 were added.)

**Coverage** (`KF4_GSN_WARM=set`, a fresh data folder, 20 s of play after a teleport): pipelines made in play
outside the set.

| settings | map | the set | outside it |
|---|---|---|---|
| the owner's (`fast`, 4x, MSAA 2, colour high, 169.83 Hz monitor) | 000, 009, 063 | 184 | 0 |
| `fast`, 3x, MSAA 8, colour high, 240 | 022 | 177 | 0 |
| `modern`, 4x, MSAA 1 | 000 | 73 | 0 |
| `native`, 3x, MSAA 4 | 000 | 129 | 0 (3 before `nd_program_of` and the resolve pass) |

ASan and UBSan (`out/linux-clang-asan`, the owner's settings, map 000, the set alone): nothing from this code (the
known underflow in `SoundDriver_Remote`; Granite's `memcpy` of no bytes from null for any pass without colour, the
game's own too).

**The over-build is accepted** (the owner, 2026-10-28: comprehensive over small). The set is a superset, 184 at the
owner's settings against 35 recorded there; a cold start pays for it once per driver or shader change (36 s on 8
threads, play waiting for what the intro and title have not hidden). An entry is narrowed only where the code shows
a draw cannot reach a pipeline, never by what a tour happened not to draw.

## Step 2: the pictures between off the GS's thread

### Measured again (smooth/picture-pump: step 1 and the refresh clock, 2026-10-28)

Map 000, the owner's settings, `KF4_PRESENT_SIM=169.83`, `--frame-rate monitor`, `KF4_HITCH_MS=1`, vblanks 2000 to
3600. The machine was busy (a browser playing video, load 2-4), and the game itself was measured as before.

| a game frame on the GS's thread | ms |
|---|---|
| the frame's packets (gif) | 15.2 |
| the vblank (mostly the flush) | 7.6 |
| the recording: the frame's packets drawn a second time, for the first picture between | 7.1 (4.55 ms each in `--stats`) |
| a replay of the recording | 0.0006 a picture |
| **in all** (median, p90, p99, worst) | **31.2**, 34.7, 36.4, 38.0 of 33 |

In play: 160.4 pictures a second (lowest second 154) and 8.4% of refreshes not one step of the world, against
169.1 and 0.9% in the table above. The difference is the load, which is the finding: the GS's thread has about 2
ms to spare a game frame, so anything else on the machine takes pictures between. The pictures left out are
mostly in the vblank of the burst.

What this changes. The plan below (2b) has the back end make the pictures between, so that none waits behind the
GS's thread's next packets. But a replay already costs nothing; what the late pictures wait for is the recording,
made on the GS's thread after the frame's burst (23-27 ms, then 7 ms more). The pump cannot make a picture before
its recording is there, so on its own it would not take this lateness away. So step 2 is now in two parts, the
recording first (agreed by the owner, 2026-10-28).

### 2a: the recording made from the frame's own drawing

The frame is drawn from its packets once into the buffer not shown, and then once more, rewritten by
frame_interp.c, as the first picture between: recorded as it is drawn (Cmd, Recording), and its kicks set up to be
moved (InterpKick, gs_replay.vert). Instead, the frame's own drawing is recorded, and its replays made into the
buffer shown. That takes about 7 ms of 31 off the GS's thread each game frame, and the pictures between can start
as soon as the frame is drawn.

What the second drawing does differently, and so what the replay must do instead (to be checked first, before a
header):
- Its draws go into the buffer shown (FRAME), and TEX0 and BITBLTBUF where they read the frame point there too:
  at a replay, the frame's images swapped for the shown buffer's, as the replay already swaps the targets' images
  (`ReplayJob::images`, `now_image`).
- Local-to-local transfers into anything but the frame are not started, but for the copies of the frame it reads
  after them and the map change's COPY_RESTORE; local-to-host ones neither; FINISH, SIGNAL and LABEL are not sent.
  The frame's drawing has them: each kept out of the recording (marked as the GS records it), or made again as a
  copy of the swapped images.
- Its vertices are drawn by gs_replay.vert from kicks, the frame's by the usual programs. Either the frame's draws
  record their kicks too (and its own picture is then drawn as now, its pixels the same), or the recording keeps
  both programs. The game's own frames must stay the same pixels with the frame rate above 30 or not (frame_test
  `--dump-frames` of the even vblanks, as now).
- The recording is checked against the GS's state before it (`interp_cannot_replay`: targets, registers, VRAM):
  for the frame's own drawing that state is the one before the frame, which the GS has again only if nothing
  between drew differently (`port_interp_end_vblank`'s exact restore).

If the frame's drawing cannot be recorded for a frame (a read-back, a cut), the picture is drawn from the packets
and recorded as now.

**Checked (2026-10-28): not as written.** Two things stop it:
- **The moves come after the frame is drawn.** frame_interp.c makes the frame's `GsnInterpMove`s at its first
  picture between (`port_interp_picture` → `gpu_frame`), after the vblank, from the whole frame and the one
  before (`objects_pair` pairs both ways). The GS draws the frame's packets as they come, before that. But the
  recording's draws are decided from the moves (`interp_moving`, `interp_place_at_0`, `interp_alpha_range`,
  `interp_t_at_0`: the scissor, the blending's copies at t 0, the alphas, the texture rows, the Z). Recording the
  frame's own drawing means holding its packets until the vblank (losing the overlap with the game's sending) or
  deciding all of those as if anything could move anywhere, in the frame's own drawing too.
- **The recording is not the frame's drawing with its draws' vertices swapped.** 89 places in the GS decide
  otherwise while recording (`interp.active`: native draw's six paths, the scene's targets, the copies, the
  corners made on the GPU, the PC world). Each would have to make both, for the frame and for its pictures.

### 2a, instead: the recording made cheaper (the owner's choice, 2026-10-28)

The same recording, the same pictures, less of the GS's thread.

**Built: a frame recorded once** (`interp_settle_newer`, gsn.cpp). A frame's recording that began with the frame
buffer's multisampled image newer and ended with its 2D drawn into the image found the GS "not as recorded" at its
next picture (`--stats`: "drawn from the packets again ... (targets)") and was recorded again: 367 of the first 1,400
frames of tests/newgame.pad, 4 ms each. Now the two images are made the same first (a resolve or a load, exact) and
named newer as before the recording. In play on map 000 the frames were already recorded once (none of the 367 is
after the teleport); at the new game's start, a quarter of the frames lose a second recording. `--stats`: "with a
multisampled target's images made the same first". Checked: 1,017 pictures (every 7th vblank to 2,420 at 170, every
picture of it, 299 of them right after a settle) byte for byte the old build's; ASan and UBSan over the same run,
nothing from it.

**Where a recording's 4.2-4.5 ms goes** (map 000 in play, the owner's settings, `--fast`, 170; `--stats`' timers
over the recordings, and 400 samples of `kf4-gs`'s stack with `eu-stack`, 125 of them in a recording):

| | share of a recording |
|---|---|
| native draw's draws (`nd_register`) | 74% |
| of it, the skinned models (`nd280_draw`): each batch's A+D writes, which end the batch before (`gs_write` → `flush_batch`) | 28% |
| the batches drawn (`sa_draw_batch`, `fast`'s), from native draw's and the GS's | 31% |
| the uploads asked for again (`replay_upload`, `sa_upload_replayed`) | 13% |
| native draw's own planning (`ns_nd_plan`) | 1% |

So what is left to cut is in native draw's draws as a picture between makes them, chiefly the skinned models'
batches: a picture's corners already come from the frame's (`nd_prep`: "taken from the frame's draws"), but each
batch's state is still written and flushed through the GS one batch at a time.

### 2b: what it needs (checked 2026-10-28)

A replay needs more of the GS's than the recording and the phase: it draws into the GS's own targets (the buffer
shown, the scrolling textures' targets, the reflection), found as the GS has them at that point of its stream
(`interp_cannot_replay`'s `replay_images`), and the buffer shown is the one the next game frame draws into once
the game swaps. So the back end cannot make a picture whenever one is due while the GS's thread works through the
next frame's packets: it would draw into the targets that frame is drawing. The pump needs the pictures between
drawn into images of their own (each target the recording writes, a copy of each it only reads, and its own
picture for the scanout), named through `ReplayJob::images` as now.

Checks: the pictures between the same as today's at each phase (frame_test with `--renderer native` and `fast`,
the map tour's pictures between, `KF4_INTERP_STILL`); the GS's thread's time a game frame (`pace_test.py --mode
cost`, and `KF4_HITCH_MS` in real time as above) before and after.

### 2b: the pictures between, made by the back end (the picture pump)

After 2a. It still matters for step 3 (unlocked), and so that no picture waits behind the GS's thread's next
packets once its recording is made.

Today the controller's thread works out each vblank's pictures between (`port_interp_schedule`, up to 16) and the
GS's thread sends each one's replay. A burst on the GS's thread (item 4) delays or cuts them. The replay is
already one call in the back end's stream (`OP_REPLAY`: the recording, shared, and the picture's buffers), and
costs the GS's thread under a microsecond.

So: the back end's thread makes the pictures between itself, from the newest recording, whenever one is due,
while the GS's thread works through the next game frame's packets. When a picture is due comes from one policy,
asked by the pump (header first, for agreement, per the C/C++ rules):

```c
/* port/include/port/picture_clock.h: when the next picture is due and which moment of the game it shows.
   One implementation a mode: a fixed rate, the screen's refreshes (the refresh clock), unlocked, capped. */
typedef struct {
    double due_s;    /* when the picture is to be handed to the presenter (port_wall_seconds) */
    double world_s;  /* the moment of the game's world it shows (from which the phase between frames) */
} PictureTime;
```

with the GPU's pictures in flight held to two (a timeline semaphore the pump waits on). The GS's thread then only
records game frames, and keeps one game frame's budget (33 ms) on average instead of one vblank's at its worst.
Also: the refresh clock's latency from a measure that a hitch does not move for the next 256 pictures (item 5),
a median with an allowance, decaying.

What to check first: that a replay needs nothing of the GS's thread's but the recording and the phase (the
corners are moved on the GPU, `gs_replay.comp`). Anything else it reads moves into the recording.

Checks: the pictures are the same as today's at each phase (frame_test with `--renderer native` and `fast`, the
map tour's pictures between, `KF4_INTERP_STILL`); the TSan build (`out/linux-clang-tsan`) over a pace run;
`pace_test.py` at 170 and 240: 99.5% one-step refreshes at 000, 009 (its bursts), 063.

## Step 3: `--frame-rate unlocked`

One more policy for the pump (step 2), not a new path:

- **unlocked**: a picture whenever the GPU has room (two in flight), each showing the world at the time it is
  expected on screen (now and the measured latency). The swapchain immediate where tearing is allowed, mailbox
  otherwise (no tearing; only the newest picture before each refresh is shown, for the least latency).
- **a cap** (`--frame-rate-cap N`, and the settings page): unlocked, no faster than N. For VRR screens (FreeSync,
  G-Sync) the cap a few below the screen's rate keeps it in the VRR range.
- The settings page: 30, 60, monitor, unlocked, and the fixed rates under "other"; a fixed rate that is not the
  screen's says it will judder (item 3).

Without VRR and without tearing, a screen shows one picture a refresh, and `monitor` is already the smoothest it
can be; unlocked then gives less latency, not more smoothness. With VRR or tearing it is smoother. The
pictures a second it reaches are set by step 4: about 285 at 4x MSAA 2x today.

Left as they are, as at every rate above 30: flipbook animations and the HUD's orb change 30 times a second.

## Step 4: the GPU's time a picture to 1.5 ms

The world in `fast` is 21,000 triangles a picture; what costs is the passes around them (item 6). In order of
what it saves:

1. **Shader blending without ending the pass.** 156,137 of 193,234 passes end for it. First a count by blend
   equation (`--stats`); then the equations the world uses mapped onto Vulkan's blending where `fast` may differ
   a little (dual-source blending covers most of `(A - B) * C + D`; reverse subtract the rest), the remainder
   drawn in the pass from an input attachment as the runs are now.
2. **One scene pass a picture.** The scene's draws, Z-first draws, skinned groups and shadows in one
   multisampled pass with one resolve; the GS's 2D and screen passes after it, at the window's pixels. This takes
   the MSAA loads (0.28 ms) and most resolves (0.49 ms).
3. **The Z-first alpha-tested draws** (0.38 ms): alpha to coverage at MSAA, or the depth test alone where the
   texture's alpha is only 0 and 1.
4. **The back end's time a draw** (`radv_CmdDraw` on RADV, PS epilogs from dynamic blending): batches of one
   material drawn with one indirect draw; the pictures between's command buffers kept and submitted again with
   only their phase changed.

Checks: RENDERER.md's (gsn_draw_test, the map tour with the scanouts, frame_test `--renderer native`), and
`fast` against `modern` by how much it differs at every tour spot. `KF4_GSN_GPU_TIME=2` before and after each.

## Step 5: stalls outside the renderer

1. **Map changes.** Measure door and area changes (`KF4_HITCH_MS=25`, `[hitch] game vblank`: disc reads, meshes
   parsed and hashed) at a dozen MAP:PLACE spots; then read the disc image ahead into memory, and parse and hash
   new meshes on a worker thread while the game's own load runs.
2. **The meshes hashed each game frame** (item 7): know an unchanged mesh by where it is and VRAM's write
   tracking, hashing only what changed.
3. **The first picture of each game frame** is drawn from the packets (4.8 ms of the GS's thread each, 2,033 in
   the 000 run; 367 drawn again as the GS was not as recorded): after step 2, measure whether it still matters.

## Order and what the owner decides

Step 0 and step 1's 1-3 first: they are small and take the stalls the owner sees now. Then step 2 (the larger
change: 2a, the recording from the frame's own drawing, then 2b, the pump), step 3 on it, step 4, step 5. Each step lands with its checks, `KF4_DATA_DIR` a temporary folder in
every run, and its numbers here.

For the owner:
- Step 1.4: a pipeline not made yet in `fast`, the batch left out for a few pictures (proposed) or the stall.
- Step 3: unlocked's default without VRR is mailbox (no tearing; **the owner's choice**, 2026-10-28); immediate
  stays an option. Whether fixed rates leave the settings page's main list is still open.
- The targets above.

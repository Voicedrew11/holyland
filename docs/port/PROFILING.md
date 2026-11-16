# Profiling: where the time goes

Shift+P opens the profiler: one window with the pictures shown against the screen's, the pipelines a draw waited for,
the frames (below) and, last, the profile run's functions. Three tools, from the broadest to the finest:

| | what it says | where |
|---|---|---|
| **Frames** (Shift+P) | picture by picture: every thread's and the GPU's timed zones on one clock, nested; spikes; self and inclusive times | in game, `KF4_PROFILE`, `KF4_PROFILE_OUT`, `tools/sampler/frames_report.py` |
| **Profile run** (Shift+P, "Functions and GPU") | each thread's **functions** by share of its CPU time; the **GPU's** time a picture by kind of work and by **shader state** | in game, `--profile-at`, `tools/sampler/profile.py` |
| Radeon GPU Profiler / RenderDoc / Nsight | per-draw, per-instruction GPU detail | outside the game, with `KF4_GPU_LABELS=1` |

## Frames (2026-11-15)

Where each picture's time went, thread by thread and on the GPU, in the order it happened. Built after Verdite 2's
frame profiler (Verdite Core's `FrameProfiler`, `ProfilerPanel`), with the GPU on the same clock as the threads.

**In game:** Shift+P, and recording runs while the window is open (F1 for the mouse).

- A bar a picture: its length (blue, red past the spike line), the GPU's busy time in it (orange), the screen's
  budget (green). Click a bar to pick that picture; **Newest** goes back to following the newest.
- **Spikes**: the pictures over the threshold (auto: twice the median and 2 ms), newest first. Click one to pick it.
- **Timeline**: the picked picture with a quarter of its length either side, every lane that did something, each
  zone nested under the one it ran in. Wheel zooms about the mouse, drag moves, double-click shows it whole; hover a
  zone for its name and length. The green lines are the picture's edges (two presents).
- **Zones**: the picked picture's zones, or each zone's average a picture over the window; **self** is a zone's own
  time (the column adds up to its lane's busy time), **incl.** adds the zones inside it, **calls** its begins, **most
  self** the largest in one picture of the window. Filter by name and by lane; **Copy table** copies it as text.
- The lane table: each lane's ms a picture working, waiting (a zone of kind wait on top) and, for the GPU's lanes,
  busy. A lane's time with no zone open is idle and counted nowhere.
- **GPU** menu: the GPU's lanes by kind of work (as `[stats]`), also by shader state, or each draw (a timestamp at
  every batch: slows the GPU, so compare its totals with by kind).

**Without playing** (any run, a window or not):

```sh
KF4_PROFILE=1 KF4_PROFILE_OUT=frames.csv KF4_PROFILE_SPIKE=12 out/linux-clang/kf4 --disc ... # + KF4_GPU_DETAIL=1|2
python3 tools/sampler/frames_report.py frames.csv --skip 2000 [--lane kf4-pc-pump] [--compare other.csv]
```

`KF4_PROFILE=1` prints every 5 s the pictures a second, their lengths and the largest zones in ms a picture;
`KF4_PROFILE_SPIKE=MS` a line for each picture longer than MS; `KF4_PROFILE_OUT` a CSV row for every zone of every
picture (`frame,start_ms,lane,zone,kind,self_ms,incl_ms,calls`, and a `frame` row with its length).
`frames_report.py` gives each lane's zones a picture, the slowest pictures and what they spent their time on, and with
`--compare` a second run's beside it zone by zone.

**How it works.** A zone is a name registered once (`port_zone_id`, `port/include/port/zones.h`) and timed with
`ZoneScope` (C++, `zone_scope.hpp`) or `port_zone_begin`/`port_zone_end` (C). Each thread writes its begins and ends
into a ring of its own (one 64-bit word an event: id, end, ns; 8 MB, a million events) without a lock. Each GPU timing
source (`pcr::GpuTimes`, one per kf4-pc and kf4-pc-pump) has a lane of its own: the timestamps are put on the CPU's
clock with calibrated timestamps (`VK_KHR_calibrated_timestamps`: `CLOCK_MONOTONIC`, or the performance counter on
Windows; calibrated again every 240 collects), a run of one kind of work one zone, each shader state (or draw) a zone
inside it. "kf4-profiler" (`port/platform/frame_profiler.cpp`) reads every ring 20 times a second into frames
(`zone_record.cpp`): a frame is the span between two presents (`port_zone_picture`, `present_vk.cpp`), a zone still
open at its edge is cut there. A frame is done once every GPU lane is past its end, or half a second after it. Done
frames are kept 10 s, the timeline's spans 2 s (the picked picture's for as long as it is picked).

**What it times.** The game's thread (its work between vblanks, its time in the vblank, disc reads), the DMA
thread (each channel's transfer, each VU1 program by its address, HLE or interpreted), kf4-pc (its inbox wait, GIF
packets, pictures and pictures between, the canvas's main functions: `draw_batch`, `room_for_planes`,
`bind_texture`, `flush_batch`, `submit`, `upload` ...), the pump (its waits, pictures between, `draw_again_alone`),
`next_frame_context` and the other waits for the GPU, pipelines made while a draw waited (Granite's compile hook,
`port_zone_after`), the presenter (the fence and image wait, the overlay, submit and present) and the profiler's own
reading. To see inside a function, give it a zone: a `ZoneScope` at its top and its id at namespace scope.

**What it costs.** Closed and with no `KF4_PROFILE*`: one relaxed load a zone. Recording, map 009 at the owner's
settings, a real window (one run each, 2026-11-15): 9,778 pictures in 54 s against 10,230 not recording (about 4%);
the pump writes about 2.5 million events a second; "kf4-profiler" spends about 1.1 ms a picture on a core of its own.
GPU detail by draw costs the GPU more (a timestamp a batch). The panel draws inside the picture it measures (the
presenter's "overlay drawn" zone). "events lost" in the window or the log names a lane whose ring filled before it was
read (it should never show); "unmatched" counts ends with no begin.

**What it cannot see.** Threads with no zones (Granite's pipeline threads, the audio): use the sampler below for
those and for functions without a zone. A GPU without calibrated timestamps has no GPU lanes (the log says so).

Until 2026-11-15 the panel's top half showed each thread's busy share and named what bounded the rate, from counters
only the removed native GS fed (with the PC renderer most stayed at 0); they and its stalls list were removed when the
frames took their place.

## A profile run

**In game:** Shift+P, then F1 for the mouse, open "Functions and GPU (profile run)" (the window's last section) and
press **Start**. Play the
part to look at, then **Stop** (or keep it running: the tables update twice a second). The thread list is by CPU
time; pick one to see its functions:

- **self**: the share of the thread's CPU time spent in the function's own code.
- **incl.**: the share spent in it or anything it calls. Click a column to sort by it.

**Save** writes the run to `<data folder>/captures/profile_<vblank>_<time>.txt`. **Copy** puts the tables on the
clipboard as text. Tick **GPU by shader state** to time the PC renderer's draws by shader state as well (below).

**Without playing:** `--profile-at VBLANK:SECONDS:FILE` starts a run at a vblank and saves it SECONDS of game
vblanks later; `KF4_GPU_DETAIL=1` turns on the shader-state timing for it. `tools/sampler/profile.py` does the
whole thing at a map with the owner's picture settings (the same run as `pace_test.py`):

```sh
python3 tools/sampler/profile.py --exe out/linux-clang/kf4 --map 9 --seconds 15 -- --renderer pc
python3 tools/sampler/profile.py --exe out/linux-clang/kf4 --map 9 --gpu-detail -- --renderer pc
```

Pass `--exe`: `pace_test.find_exe` picks `out/linux-gcc` first if one exists, and that may be an old build.

**Digging into a saved run** with `tools/sampler/stacks.py` (it names our frames with `llvm-symbolizer`, inlined
functions as frames of their own, so it sees more than the panel):

```sh
S=~/.config/holyland/captures/profile_....txt; K=out/linux-clang/kf4
python3 tools/sampler/stacks.py $S $K --threads                   # every thread, then the busiest's functions
python3 tools/sampler/stacks.py $S $K --thread kf4-pc-pump --top 60
python3 tools/sampler/stacks.py $S $K --thread kf4-pc --callers draw_batch --depth 4
python3 tools/sampler/stacks.py $S $K --thread kf4-pc --callees feed
python3 tools/sampler/stacks.py $S $K --gpu                       # the GPU's tables
```

The threads: `kf4` (the game's, on its fibers), `kf4-dma` (DMA, VIF1, VU1), `kf4-pc` and `kf4-pc-pump` (the PC
renderer and its pictures between), `kf4-between` (paces the pictures between), `kf4-gs` / `kf4-gs-back` (the
native GS), `kf4-sampler` (the profiler, never sampled itself).

## How the CPU side works (Linux)

`port/platform/sampler.c`. A thread of its own wakes every millisecond. For each of the process's threads that is
running at that moment ('R' in `/proc/self/task/TID/stat`), it sends that thread `SIGPROF`. The handler, on the
interrupted thread, walks the frame-pointer chain and hands the return addresses back. A thread waiting in a system
call is never interrupted, so the profiler does not wake sleeping threads, and every sample is a moment of CPU time:
a function's share of a thread's samples is its share of that thread's CPU time.

No root and no `perf` are needed (works at `perf_event_paranoid` 2).

- **Frame pointers.** The walk needs them. `-DKF4_FRAME_POINTERS=ON` (the default on Linux) keeps them in all our
  code. Fedora builds its own libraries with them, so the Vulkan driver, SDL and libc walk too. A library without
  them shows as its innermost frame and its caller's chain may be cut short.
- **Names.** Functions are named from each module's ELF symbol table (`port/platform/elf_symbols.c`). For Fedora's
  stripped libraries, the symbol table compressed in `.gnu_debugdata` (MiniDebugInfo, read with liblzma) names
  their static functions too: `radv_emit_all_graphics_states`, not just exported `radv_CmdDraw`. Code no symbol
  covers shows as `[module]`.
- **Cost** (measured 2026-11-07, map 009 at an uncapped 500, `--renderer pc`, 3 runs each, about ±5 pictures a
  second between runs): frame pointers on 241 pictures a second against 246 off (about 1.8%); recording a run 238
  against 241 not recording (about 1.5%). Nothing while not recording. At a 240 cap no difference shows.
- **Windows.** There is no in-game sampler there; `tools/sampler/profile.py` runs `kf4-sampler.exe`
  (`tools/sampler/sampler.c`) from outside, which unwinds stacks from the program's `.pdata`.

## How the GPU side works

Both renderers write GPU timestamps where the kind of work changes: the PC renderer in `port/gs/pc/pcr_gpu_time.cpp`,
the native GS in `gsn_backend.inc`. While a run records, each interval goes to the profiler's GPU table
(`port_profile_gpu_add`, `port/include/port/profile.h`), named "pc native 280", "native GS pass" and so on. The
kind rows add up to the GPU's time and agree with `--stats`' `GPU time` line.

**By shader state (PC renderer):** with detail on, a timestamp is also written each time a batch's shader state
changes, and the interval goes to that state's row, e.g.
`native 280 | sampled modulate rgb | fog | blend (Cs-Cd)*As+Cd | Z >= write`. These rows add up to their kinds'
draw rows. A state is what changes the shader's work: the program (2D or native 000/280/700), the texture source
and function, fog, the GS blend (A-B)*C+D and whether the shader does it, DATE, the Z test and write, the alpha
test's passes. "runs" is the number of timed intervals. A timestamp per change costs some GPU time (hundreds a
picture), so detail is off by default; compare totals with it off.

**Per-draw, per-instruction:** `KF4_GPU_LABELS=1` marks each kind of work as a debug label (VK_EXT_debug_utils) in
the PC renderer's command buffers, so outside tools show named regions. With RADV:

```sh
# Radeon GPU Profiler: a capture on a trigger file (Mesa's RGP support, MESA_VK_TRACE)
KF4_GPU_LABELS=1 MESA_VK_TRACE=rgp MESA_VK_TRACE_TRIGGER=/tmp/kf4-trigger out/linux-clang/kf4 --disc ... --renderer pc
touch /tmp/kf4-trigger      # in play: the next frame is captured to /tmp/*.rgp; open it in RGP
```

RenderDoc (`renderdoccmd capture ...`) shows the same labels in its event browser.

## Checking it still works

`ctest -R zones`: the rings read while written (and losses counted), and the frames' self, inclusive and call
counts cut at the pictures, GPU lanes that come in late. `ctest -R sampler`: the counts, the ELF reader (on the test's own file, and the file cut short at every few KB),
and a live run that must find a busy thread's function at 90% self or more. `ctest -R profile`: the GPU table.

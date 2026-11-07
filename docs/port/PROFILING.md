# Profiling: where the time goes

Three tools, from the broadest to the finest:

| | what it says | where |
|---|---|---|
| Shift+P panel, top half | each thread's busy share, the GPU's ms a picture, what bounds the rate, stalls | in game |
| **Profile run** (Shift+P, "Functions and GPU") | each thread's **functions** by share of its CPU time; the **GPU's** time a picture by kind of work and by **shader state** | in game, `--profile-at`, `tools/sampler/profile.py` |
| Radeon GPU Profiler / RenderDoc / Nsight | per-draw, per-instruction GPU detail | outside the game, with `KF4_GPU_LABELS=1` |

## A profile run

**In game:** Shift+P, then F1 for the mouse, open "Functions and GPU (profile run)" and press **Start**. Play the
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

`ctest -R sampler`: the counts, the ELF reader (on the test's own file, and the file cut short at every few KB),
and a live run that must find a busy thread's function at 90% self or more. `ctest -R profile`: the GPU table.

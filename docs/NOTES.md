# KFIV-PC work log — SLUS_203.18 (King's Field: The Ancient City, USA)

Per-game memory: current blocker, findings, and ruled-out avenues. Rewrite in
place as state changes; link session logs at the point they support.

## Current state (2026-10-09)

- **Native Windows:** the USA build reaches the first gameplay area with
  Verdite keyboard/mouse controls, repeatable inventory entry/back,
  pause/resume and the capture-state glyph. Windows x64 MSVC 19.44,
  `RelWithDebInfo`, `/O2 /fp:strict`, with FFmpeg enabled, was verified.
  W changed player coordinates; Space reached raw and guest Square edge
  state. Attack animation/damage was not independently measured.
- **Audio:** music, effects and opening audio run through the original
  SDRDRV/LIBSD modules and the new SPU2/host output. The earlier 248.218-second
  isolated run reached tick 10,600 and exited normally after natural movie
  completion and entry into gameplay. Its opening had no audio underruns;
  final sampled counters showed 256 underrun frames after entering 3D,
  with zero dropped frames. An installed 44.338-second keyboard smoke run
  displayed the opening forest with zero sampled underruns/drops and no
  clipping in its 43.84-second PCM capture. Earlier source/device/process
  captures also showed no clipping.
  The final 137.034-second gameplay-rendering check had zero sampled
  underruns through the movie, then 2,304 cumulative underrun frames after
  pause/3D gameplay, with zero drops. Underrun-free gameplay is not established.
- **Movie presentation fixed:** decoded RGB already reached the correct
  GS texture, but disabled depth testing still applied `ZTST=NEVER` and
  rejected the original movie sprite. Patch 0023 honours `TEST.ZTE=0`
  and suppresses depth writes, so opening pictures and audio now play.
- **Gameplay strip alignment fixed:** traced framebuffer feedback sprites
  reversed both axes, but the rasterizer sorted XY without matching UV,
  mirroring each 64-pixel strip. Patch 0024 preserves paired endpoints,
  fractional XYOFFSET/UV, ceil-exclusive coverage, integer GS sample
  positions and the second vertex's flat Q. The corrected preview reached
  tick 4,500 normally in 128.879 seconds; walls and sky align continuously
  at the same player position as the banded baseline. The texture-cache
  policy and presentation remain unchanged by this fix.
- **Bounded diagnostics:** patch 0025 makes every opt-in GS trace event
  respect its tick interval, includes the active CRT2 registers, and reports
  fractional XYOFFSET correctly. It changes diagnostics rather than pixels.
- **Gameplay source height:** `DISPLAY2.DH=895` encodes 896 display lines
  in interlaced FRAME mode. The early 512-row cap reduced this to 256 source
  rows and triggered false field doubling. Patch 0026 converts the height
  before capping it, retaining all 448 gameplay rows. The movie's existing
  224-row field path and the general weave implementation are unchanged.
- **Installed gameplay verification:** the final build reached tick 4,800
  normally in 137.034 seconds, with 81 captures at 640×448 showing the full
  scene and HUD. W movement, camera turning, inventory entry/back and
  pause/resume were verified; opening movie pictures remained visible.
  The installed executable, 28 matching DLLs and shortcut use that build.
  This is visible integration evidence, not full PS2 pixel equivalence.
- **Remaining problems:** interlace combing appears in some movie frames,
  broader 3D rendering accuracy remains unverified, and gameplay is below
  full speed.
  Audio hardware details are approximate; later areas, a full playthrough,
  Windows controller integration and a real save/load roundtrip are unverified.
- **Opening-screen controls:** after accepting brightness with F, the
  movie has about 92 seconds of pictures and audio. Enter optionally sends
  the original Start input to skip it; the original movie cleanup and area
  loader still run. Earlier black-picture builds could look like a hang
  with PC audio muted. PR publication did not itself change the executable.
- **Source delivery:** existing patches 0001–0014 are unchanged. New
  source-exported patches 0015–0026 include Windows build/input, loader
  completion, capture UI, timing, native audio/MPEG, GS depth and sprite fixes,
  source-height decoding and bounded diagnostics. The 26-patch series has
  twelve additions affecting 43 source/license/build files. Generator hunks must be applied to tools
  before generating the game; rebuilding only the runtime leaves stale
  continuation registration.
- **Verification:** the repository now holds 20 standalone fixture packages.
  The original 16 passed 26 CTest entries on Windows, along with seven
  patch-helper tests and earlier full-series application/reruns. The new
  GS depth fixture passed 143,397 checks and 2/2 CTest entries; focused
  upstream GS checks passed 51/51. At that depth-fix stage, the legacy GS suite was 41/72 with
  the same 31 failures in fixed and prior controls. New sprite checks passed
  853,552 assertions with one and eight workers; the prior-sprite control
  reproduced 46,839 failures and 217 artificial strip-boundary steps.
  The trace fixture passed 59 assertions in nine fixed cases, with six
  old-behaviour controls and 15/15 CTest entries. Source-height checks passed
  6,129,642 assertions in nine fixed cases; six pre-height controls reproduced
  4,072,731 failures in 4,081,608 checks, with 15/15 CTest entries.
  All three packages were rebuilt after relocation. Fresh tools/full/repeat
  application of all 26 patches reproduced the source-export tree
  `6c3edbccc9e71f1477863bd707e5758658e9b28b`, with all 57 managed files
  verified after normalization. Details and limits are in
  [`windows-validation.md`](windows-validation.md); reproduction steps
  are in [`windows.md`](windows.md).
- **Upstream:** the reusable continuation-ownership fix is submitted as
  [PS2Recomp #279](https://github.com/ran-j/PS2Recomp/pull/279). It extends
  the merged standalone-entry fix #271 to overlapping wrappers, protects
  explicit handlers and makes alias ownership deterministic. The pinned
  Holyland series still needs its generator hunks until the pin moves.
- **GS upstream:** the generic disabled-depth fix is submitted separately as
  [PS2Recomp #280](https://github.com/ran-j/PS2Recomp/pull/280).
  The sprite-coordinate fix is submitted as
  [PS2Recomp #281](https://github.com/ran-j/PS2Recomp/pull/281).
- **Next work:** interlace presentation, 3D rendering correctness/performance
  and broader game-path validation. The earlier Linux measurements and
  findings below remain historical; the new series has not been retested
  on Linux.

## Earlier Linux state (2026-10-03)

- **Stage:** reaches gameplay. Logos, title screen, menus and Brightness
  render correctly; New Game goes through the opening-movie code path and
  into the first 3D area with the HUD. Confirmed on the maintainer's machine.
- **Known problems:** opening movie shows black (MPEG HLE); 3D graphics
  partly wrong; in-game below full speed (~13 fps of 30 in the first 3D
  area, see Performance); the game auto-pauses shortly after entering the
  3D area with no input (pad state suspected); no audio (SPU2 not emulated).
- **What fixed the title screen:** the analyzer had stubbed libgraph's
  `sceGs*` functions with HLE versions that use invented struct layouts
  (`sceGsExecLoadImage` read a 12-byte fake struct, so textures were uploaded
  to the wrong VRAM). They are now unstubbed in `kfiv/config.toml` (also all
  `sceVu0*`), so the game's own SDK code runs. That exposed a GS bug: GIF tag
  state must persist across DMA transfers (patch 0002). The old patches
  "NLOOP=0 means 32768" and "CT24 uses 32-bit words" were workarounds for the
  broken stub and were wrong; they are gone.
- **What got it in-game:** `sceCdDiskReady@0x0022EF30` binding (the analyzer
  missed it; the game spun binding RPC 0x8000059A), IOP heap reuse + IOP
  `ioman` file I/O (patch 0006), handler stacks off the main stack (0010),
  and 411 functions Ghidra missed added to the function map with
  `scripts/maintainer/fill-map-gaps.py` (code only reached through function
  pointer tables; symptom `missing-target ... op=JALR`).
- **Next steps:** 3D rendering correctness, performance (VU1 interpreter,
  see Performance), the auto-pause, MPEG playback, SPU2 audio.
- **Unpublished experiments** (exist only in the maintainer's local
  PS2Recomp checkout, not in this repo; ask the maintainer if you want
  one): AI-generated, untested, based on older states of the series.
  `agent/raster` (rasterizer audit), `agent/dmaaudit` (libdma stub audit),
  `agent/audio` (a first SPU2 implementation: 2 cores x 24 voices, ADSR,
  ADPCM, raylib audio output). Treat them as hints of where to look, not
  as known-good code.

## Performance (2026-10-03)

Benchmark: headless run into the first 3D area, `PS2X_STATS=1`, average
display flips/s over ticks 3000–3600 (the game renders one frame per two
VSyncs, so full speed is 30). Ryzen 7 5700X (8 cores / 16 threads), dev
(-O2, non-LTO) build.

| State | Game fps |
|---|---|
| Before (patches 0001–0010), dev build | 3.0 |
| + asynchronous parallel GS (0013) and part of 0012, dev build | ~8.2 |
| + rest of the VU1 work (0012), dev build | 13.0 |
| Same, Release (LTO, -O3) build | 13.8 |

(Intermediate steps were also measured on the intro window, ticks
2450–2750: synchronous parallel GS alone took it from ~2.7 to ~5.5.)

- Where the time went at the start (one thread did everything): GS
  rasterization ~57%, VU1 interpreter ~35% (mostly per-cycle pipeline
  bookkeeping, not arithmetic), EE game code <10%. Per game frame: ~1,000
  VU1 microprograms, ~800k VU1 cycles, ~10k primitives, ~4M pixels (13x a
  640x448 screen: heavy full-screen multipass effects).
- Now: the EE thread is ~85% VU1 interpreter (flat profile, no dominant
  line). The GS workers are idle or at barriers more than half the time,
  so the GS has headroom; it will matter again once VU1 is faster.
- **VU1 census (2026-10-09, dev build, run to tick 3300):** the whole run
  uses **one** VU1 microcode image (16 KB, a single content hash) with four
  entry PCs: 0x0000 (85% of VU1 cycles, 41k MSCALs + 391k MSCNT resumes),
  0x1400 (13%, 4.8k + 167k resumes), 0x3800 (0.8%) and 0x13b0 (0.3%). The
  programs run in E-bit-terminated segments (each ends with a pipeline
  flush), so every segment starts with quiescent pipelines. That makes an
  ahead-of-time VU1 recompiler tractable: one image, two hot entries, known
  entry state. Instrumentation: `PS2X_VU1_CENSUS=<file>` (patch 0027).
- **VU1 lift (2026-10-10, patch 0028, opt-in `PS2X_VU1_LIFT=1`):**
  `scripts/maintainer/vu1lift.py` translates the MSCNT segment of entry
  0x0000 (resume pc 0x08f8: the 391k resumes, most of that entry's 85%
  of VU1 cycles) to C++ with a static
  cycle schedule; `VU1Interpreter::run` calls it when the pipelines are
  idle. Bit-exact: 1.94M microprogram runs into gameplay on the 28-patch
  series (780k on the earlier 15-patch one), 0 `[vu1verify]` mismatches
  (registers, flags, cycles, VU1 memory, XGKICK output).
  **It is not a speedup yet; keep it off.** Same build, gameplay ticks
  4000-4599 (input script under "Dev loop"):

  | GS mode | lift off | lift on |
  |---|---|---|
  | asynchronous (default) | 11.5 flips/s | 5.1 |
  | `PS2X_GS_LOCKSTEP=1` | 7.8 | 9.2 (+18%) |

  (The 15-patch series, ticks 3000-3599: 11.6 / 5.1 and 8.6 / 11.0.)

  Lockstep shows the lift removes work (one flip per two VSyncs either
  way). With the asynchronous GS the EE thread now outruns the raster
  workers: ~40% of its samples block in `GSCpuBackend::KickIfIdle ->
  WaitForCompleted` (queue full, called from `finishXgkick`), the workers
  spend more time in their barrier, and the game presents one frame per ~6
  VSyncs instead of 2 (profiled on the 15-patch series). Cause not found
  yet. Not the VBlank catch-up bursts: patch 0020 on its own changed
  nothing (5.3 vs 11.1). Next: find why the
  asynchronous GS stretches frames once VU1 is cheap, then lift 0x1400.
  A GPU GS backend (PR #10, Vulkan) would remove the raster cost the lift
  exposes.
- **GS self-feedback draws** (~45 per frame here: 64-pixel-wide vertical
  strips that sample the frame buffer they render to, at half-pixel
  offsets, a blur/glow pass) run on one worker, row by row, because the
  reference result depends on the exact row order (and on the single-page
  texture cache). Parallelising them needs a decision on the intended
  semantics (e.g. read-old-data snapshot), not just engineering.
- Trap found on the way: **host FP rounding mode.** The VU1 interpreter
  runs with `FE_TOWARDZERO` and PATH1 (XGKICK) draws used to be rasterized
  inside it, so the reference GS rasterized those draws with truncating
  float math. Executing them later on another thread changed pixels by
  ±1–3; the queued draw now carries its rounding mode. Any future move of
  work across threads must do the same.
- Comparing frame dumps at fixed ticks stops working once rendering is
  asynchronous (how many game frames run per tick depends on scheduling);
  use the recorder/replay and the VU1 verifier below instead.

## Dev loop (maintainers)

- Setup, dev build, headless runs and the dev harness variables:
  [`contributing.md`](contributing.md). The dev build
  (`scripts/maintainer/dev-build.sh`, -O2, no LTO): full build ~4 min on
  an 8-core desktop, a runtime-only change relinks in ~5 s (the
  Release/LTO relink in `03-build-runner.sh` takes 5.5 min).
- Build-time findings (2026-10-09): the Release build's compile step is
  only ~70 s because `-fno-fat-lto-objects` defers code generation to the
  LTO link (~6 min). `ps2_recomp` rewrites every output file even when
  unchanged, so anything that copies by mtime rebuilds all 889 unity
  batches; `dev-build.sh` copies by content (`rsync -c`). GCC `.gch` files
  differ between two compiles of the same input, so ccache misses every
  unity batch after the PCH is rebuilt. Fresh dev tree, 16 threads: 292 s
  with PCH and no ccache; 423 s with ccache (depend mode, PCH off) and an
  empty cache; 66 s with a full cache. Hence ccache is opt-in
  (`KFIV_CCACHE=1`). Linking the dev runner is not the bottleneck: 0.8 s
  with GNU ld, 0.13 s with mold; the "~5 s relink" is mostly recompiling
  the changed file.
- Profiling without `perf`: sample the running process with `eu-stack -p
  <pid>` in a loop (the dev harness `ctrl` file's `tick` command tells when
  the run reaches the scene); add `-g` to a few sources for line info.
- GS renderer regressions: record a session with `PS2X_GS_RECORD=<file>`
  (~2.5 GB to tick 2300), build `scripts/maintainer/gsreplay/build.sh
  <ps2recomp-checkout> <reference-rev>` and run `gsreplay <file>`: it
  replays the trace through the current backend and the reference one and
  compares VRAM at every sync point (deterministic, independent of guest
  timing; also times both backends). `PS2X_GS_THREADS=1` and
  `PS2X_GS_LOCKSTEP=1` narrow down threading problems.
- VU1 regressions: apply `scripts/maintainer/dev/vu1-verify.patch` on top
  of the series and run with `PS2X_VU1_VERIFY=1`: every microprogram also
  runs on a frozen copy of the original interpreter, and registers, flags,
  cycles, VU1 memory and XGKICK output are compared (`[vu1verify]` on
  stderr; 720k runs into gameplay, 0 mismatches for patch 0012).
- VU1 lift: `vu1lift.py <image.bin> --segment 0x08f8 -o
  <game-dir>/vu1lift/<name>.cpp` (the image comes from the census), then
  `dev-build.sh` compiles it into the runner and `PS2X_VU1_LIFT=1` enables
  it. The generated file embeds game logic: never commit it. Verify every
  generator change with the VU1 verifier. Status and open work:
  [`handoff-vu1-lift.md`](handoff-vu1-lift.md).
- Reaching gameplay with the 26+ patch series (the title and new-game
  movies now play; START skips them; gameplay from about tick 3300):
  `PS2X_INPUT="400:START:10,700:START:10,1000:START:10,1300:START:10,1700:CROSS:10,2000:START:10,2300:START:10,2600:START:10,2900:CROSS:10,3300:START:10,3700:CROSS:10"`.

## Findings

- ISO `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_203.18;1`, `VER = 2.00`,
  `VMODE = NTSC`. Boot ELF `SLUS_203.18` is 3,452,268 bytes, ELF32 LE MIPS,
  entry `0x100008`. Single program header (`LOAD`, vaddr `0x100000`).
- ELF is **stripped**: `readelf -s` returns zero symbols. Section headers are
  sane (23 sections: `.text` 1.3 MB, `.data`, `.rodata`, …) — unlike titles
  with hundreds of empty sections, so no `invalid code region` spam expected.
- `ps2_analyzer` (PS2Recomp @ 2026-09-30, Linux GCC 16.2 build):
  **264 library functions to stub, 286 without runtime handlers**, 0 patches,
  0 jump tables. Stubs use `handler@0xADDRESS` form (correct for stripped ELF).
- `ps2_recomp` on analyzer config: **3208 processed / 3019 recompiled /
  189 stubs / 0 skipped / 0 decode failures / errors: 0**. Output 3218 `.cpp`,
  ~84 MB. Only warnings are `JR`/`JALR` fallback promotions (switch tables).
- Leading-underscore bindings present (`_sceSDC`, `_printf`, `_malloc_r`,
  …). Per workbench experience the runtime registers them *without* the
  underscore — normalize before trusting stub coverage; validate against
  `ps2_call_list.h` (see `maintainers.md` step 2).
- PS2Recomp Linux configure: full build with `PS2X_BUILD_STUDIO=OFF` still
  pulls raylib/GLFW (Xinerama headers missing on this Fedora box) — the GUI
  dependency comes from the runtime host backend (raylib is unconditional;
  `DEBUG_UI=OFF` only drops imgui). Analyzer+recompiler build with
  `-DPS2X_BUILD_RUNTIME=OFF` is clean. Runner needs
  the X11 dev packages listed in `building.md` first.
- Ghidra 12.1.3 + EmotionEngine extension, import 2026-09-30:
  `function_count = 3185` (analyzer: 3208 — consistent), 28008 CSV records,
  but only **47 stubs** vs analyzer's 264. Merge strategy confirmed: Ghidra
  for boundaries, analyzer for SDK names. Raw export also demonstrated the
  known trap: stale absolute paths for `output` and `ghidra_output` — never
  use it directly. Export saved as `<game-dir>/ghidra.toml` (reference only).
- 2026-09-30: runner links (83 MB) after moving generated code into
  `ps2xRuntime/src/runner/` + `include/` (replacing the default table) and
  setting `TMPDIR` under the game dir (/tmp tmpfs too small for the LTO
  link). First boot with game code: boots through EE init, IOP modules,
  GS output — no missing-targets. Two traps fixed along the way: upstream
  sets no x86 SIMD flags (`-msse4.1` needed); `04-run.sh` originally passed
  a nonexistent `--iso` flag (runner takes ELF as argv[1], no CDVD support).
- 2026-09-30: extracting the ISO into the game dir lets the IOP emulator
  load all 9 real IRXs (was: HLE fallbacks, 4 sound modules with no
  provider). `cdRoot` defaults to CWD, so `04-run.sh` now `cd`s to the game
  dir. Game now runs its main loop: dma/gif counters climb, pad polling
  live, pcs cycle 0x209d78/0x209660 (SIF-wait/main-loop). Understood the
  earlier "hang" at 0x1cb7e0: SDK `sceSifBindRpc` + poll-`server` retry
  pattern, resolved once real IRXs loaded.
- Keyboard→pad map (runtime): WASD/arrows=d-pad+stick, X/Space=cross,
  C/Esc=circle, Z=square, V=triangle, Q/E=L1/R1, shifts=L2/R2,
  Enter=START, Tab=SELECT. Gamepad also supported if connected.
- 2026-09-30: CSV saved (28,008 records), stubs merged 264+47=**311**
  (all validate against 658 runtime handlers, zero overlap, no `_` renames
  needed). Recompile with Ghidra map: **28008 processed / 27719 recompiled /
  289 stubs / errors: 0**, output 200 MB / 28k files (vs 84 MB / 3.2k
  analyzer-only). CSV mixes 3,130 FUN_ with 23,961 entry_* labels + switch
  cases — recompiler emits entry wrappers for them by design; cost is output
  size, not correctness. Median record size 28 bytes.
- 2026-09-30: user closed the game. Pad log proves input reached the game
  (varied data2/data3 incl. START bit) — mechanics fine. Menu loop sits in
  `sceGsSyncV` wait (0x209d78/0x209660) which is NORMAL vsync-paced idle,
  not a hang; present loop runs ~12fps (software GS + aggressive logging),
  so input feels sluggish. Menu makes no SwapDBuffDc calls (5-min GDB probe:
  zero hits) — static screen. The transient DISPFB ASCII corruption happened
  mid-boot and recovered. Real blocker remains: no textures/text (font atlas
  upload implicated). Next: capture the corrupt window from a fresh boot.
- 2026-09-30: menu screenshot: black bg, dark-green bar (selection
  highlight?), small blue box, NO text and NO textures — flat quads only.
  Input works (game changes screens). No audio (not implemented runtime-side).
- 2026-09-30: ROOT CAUSE + FIX. `[gs:texa]` storm = IMAGE bytes parsed as
  REGLIST: game sends a 512KB texture with GIFtag NLOOP=0 (HW: 0 = 32768
  loops); runtime treated it as 0, then parsed 512KB of texture as register
  writes (TEXA, DISPFB1/DISPLAY1 ← font bytes from KF4.DAT). Fixed NLOOP
  0→32768 in `GS::processGIFPacket`, `visitPackedGifPacket`,
  `tryProcessNativeImageUploadPacket` (gs_frontend.cpp) and `gifTagNloop`
  (ps2_memory.cpp) — spec-correct per Sony docs. Shipped as
  `patches/0001-gs-gif-nloop-zero-means-32768.patch` (applied by
  `03-build-runner.sh`). Verify run: 0 texa lines, dispfb1 sane to tick 11880, 0
  missing-targets. Textures/text should now render. **Superseded
  (2026-10-03):** this patch was a workaround for the broken `sceGs*` HLE
  stub and was removed; see "Current state".
- Earlier symptom, superseded by the root cause above: the screen turned
  magenta mid-boot. Magenta = `UploadFrame` fallback (`ps2_runtime.cpp`, blank
  `MAGENTA` texture) when `copyLatchedHostPresentationFrame` fails. Deeper:
  tick snapshots show `dispfb1/display1` turning into ASCII (`=;3PPFbf`,
  `II?Z[Off`) — bytes traced to the font atlas in `DATA/KF4.DAT:6530096`.
  So GIF-stream parsing desyncs and IMAGE (texture) bytes get executed as
  A+D writes to DISPFB1/DISPLAY1 (0x59/0x5a, which are NOT in the `[gs:reg]`
  interesting-list, hence silent). Renders fine (sprites, double-buffer
  swap 0x1400/0x148c, 640x224 fields) until the desync. Suspect: NLOOP/EOP
  accounting on a multi-packet IMAGE transfer. Next: GDB watchpoint on
  `gs_regs.dispfb1` to catch the corrupting write + backtrace.

## Ruled out / traps

- Do **not** run the raw Ghidra export alone: it rewrites `config.toml`
  wholesale (resets `skip`, writes stale absolute paths). Always
  normalize + merge analyzer stubs afterwards (`maintainers.md` step 2).
- `skip.txt` names must use the **recompiler's** `sub_XXXXXXXX` names, not
  Ghidra's `FUN_XXXXXXXX`, if a runaway boundary ever appears (none so far).
- Game data (ISO/ELF/`output/`/`_build/`) stays out of git — `git clean -xfd`
  deletes ignored files, which would nuke gigabyte-scale artifacts.

## Game profile (for later: overrides, IOP)

- IOP modules on disc: `FSIOPSND, LIBSD, MCMAN, MCSERV, MODHSYN, MODMIDI,
  PADMAN, SDRDRV, SIO2MAN, IOPRP224.IMG`. Sound is MIDI/synth-heavy
  (MODHSYN/MODMIDI) — expect audio HLE to matter early.
- Big data file `DATA/KF4.DAT` (503 MB); FMVs as `.PSS`. The runner reads
  the unpacked disc files from its working directory (`01-extract.sh`);
  it has no ISO support.

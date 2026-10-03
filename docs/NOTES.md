# KFIV-PC work log — SLUS_203.18 (King's Field: The Ancient City, USA)

Per-game memory: current blocker, findings, and ruled-out avenues. Rewrite in
place as state changes; link session logs at the point they support.

## Current state (2026-10-03)

- **Stage:** reaches gameplay. Logos, title screen, menus and Brightness
  render correctly; New Game goes through the opening-movie code path and
  into the first 3D area with the HUD. Confirmed on the user's machine.
- **Known problems:** opening movie shows black (MPEG HLE); 3D graphics
  partly wrong; in-game runs very slowly (software GS); the game auto-pauses
  shortly after entering the 3D area with no input (pad state suspected);
  no audio (SPU2 not emulated).
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
- **Next steps:** 3D rendering correctness (candidate branches from the
  rasterizer and libdma audits exist in the dev PS2Recomp checkout as
  `agent/raster`, `agent/dmaaudit`, untested on 3D), performance (software
  GS), the auto-pause, MPEG playback, SPU2 audio (`agent/audio` is an
  untested start).

## Dev loop (maintainers)

- Non-LTO dev build of the runner: configure PS2Recomp with
  `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG"
  -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF`. A full build is ~4 min; a runtime-only
  change relinks in ~5 s (the default Release/LTO relink takes 5.5 min).
- Headless runs: `PS2X_HIDDEN=1 PS2X_DUMP_EVERY=150 PS2X_EXIT_TICK=2400
  PS2X_INPUT="1100:START:10,1500:CROSS:10,1900:CROSS:10"` (START opens the
  title menu, CROSS picks New Game, CROSS confirms Brightness; in-game by
  ~tick 2400). See patch 0001 for all variables.

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
  missing-targets. Textures/text should now render.
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
- Big data file `DATA/KF4.DAT` (503 MB); FMVs as `.PSS`. Runner needs the
  **disc image** (raw sector reads), not loose files.

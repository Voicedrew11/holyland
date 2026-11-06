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

## The list, best first

1. **pc's work on two threads.** One takes each frame in; the other draws the pictures between from the frame's
   kept walk (which the intake does not change). Those are the 41% and the 45% above: up to about twice the
   pictures at 009. A design change (interface first).
2. **Fewer draw calls and descriptor writes.** About 23% of pc's thread is Granite's per-draw cost: merged draws
   (item 4 of the handoff), bindless textures for draws across texture and CLUT changes.
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

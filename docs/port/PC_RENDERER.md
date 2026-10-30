# `--renderer fast` and smooth play

One plan for how `fast` draws and how smoothly it plays. It was two documents until 2026-10-29: the PC renderer's
plan (this file: P1-P6, how the world is drawn) and the smooth play plan (SMOOTH.md: steps 0-5, pipelines, pacing,
unlocked, the GPU's time). They were judged at different rates and drifted apart: the PC renderer's plan was closed
out at 170 Hz, where every spot kept up, and the smooth play plan took the GPU for the ceiling of a higher rate. Both
parts are kept below as they were written and measured, each with its corrections marked; this top says where the
whole stands.

## Where it stands (2026-10-29)

**Built.**
- The PC renderer (part 1): P1 (hardware textures), P2 (the map's records, objects, items and gold, opaque,
  alpha-tested and blended, kept and drawn by material), P3's three steps (the skinned parts' first pass once, alphas a
  little above 1.0 by Vulkan's blending, the skinned groups on P1's textures), P5's first step (a taken draw's kept
  batches drawn again in its pictures between). P4 and P6 measured and left (nothing to win on the GPU). **P7**
  (2026-10-29, below): the world drawn from its meshes on the GPU, off the GS's batches: the map, objects, items,
  creatures, arm and effects, the material still resolved where the GS's thread meets the draw.
- Smooth play (part 2): the refresh clock (step 0); the pipeline set, the pipeline files off the GS's thread, one
  count of compiles (step 1); a frame recorded once (step 2a); `--frame-rate unlocked` as a rate found as it plays,
  `--frame-rate-cap`, `--tearing` (step 3); shader blending's copies in the render pass in `fast` at every colour depth
  (step 4, item 1).

**What limits the pictures now: the native GS's thread, not the GPU.** `fast` is not a renderer apart from the native
GS: P2 and P3 grew it inside the native GS's scene ("P2, revised" below: a draw's material is only known on that
thread, in the chain's order). Every draw still goes through that thread, and each game frame is recorded there for
its pictures between. At the owner's settings of 2026-10-28 (4x, native widescreen, MSAA 2, `--native-colour high`,
170 Hz; heavier than those part 1 was measured at: MSAA 1, colour exact), map 000:

| | |
|---|---|
| the GPU a picture | 3.33 ms: room for about 300 pictures a second |
| the GS's thread a game frame | about 31 ms of 33 (step 2's measures) |
| a recording, and what the pictures between wait for it | 4.2-5.8 ms; 10.75 ms median in the recording's vblank |
| fixed 240 | 189 pictures a second, 1,353 left out as the GS's thread fell behind (the GPU then busy about 80% of the time) |
| unlocked | settles about 175 a second, as monitor |

Below about 200 a second it keeps up (part 1's "every spot keeps 168-170 at 170 Hz" is still so), which is why part 1
was closed out as done and why it did not show as the limit. Above it, it is the first limit at these settings.

**Since P7** (the table above is from before it): the GS's thread a game frame is 18-19% less at maps 000 and 009
(13.1 and 13.9 ms in `pace_test.py --mode cost` at 240); fixed 240 keeps 239.75 a second at both; unlocked map 000
256-258 a second (was 198-203), map 009 262-264 (was 225-228). The thread is still the limit unlocked, now mostly the
recording and the draws P7 declines ("P7" below: what still goes through the GS's batches).

**Since (2026-10-29, later): two pieces of the recording.**
- **The GS's thread no longer waits for Granite's lock** (RENDERER.md, "The back end"; `KF4_GSN_IMAGES_OFF_THREAD`).
  Sampled with `eu-stack`, 28% of a recording at map 000 was the GS's thread waiting for Granite's device lock, which
  the back end's `next_frame_context` holds while it waits for the GPU: a target dropped by an upload asked for again
  (`destroy_image`), and made again by the next draw (`create_image` asking for a command buffer to make it GENERAL).
  Now the images are let go on the back end's thread and made GENERAL by a call of the stream. A recording at map 000
  (`--fast` at 240, 12,000 vblanks, alternating): 3.85 and 4.11 ms, against 5.42 and 6.76 with the switch off; the
  pictures byte for byte (maps 000 and 009, 204 each, and with `KF4_GSN_BACKEND=0`).
- **P7's calls kept for the pictures between** ("P7, kept", below; `KF4_PC_KEPT`): built, about 3% of a recording at
  000, held back by its table check.

**The owner's decision (2026-10-29, evening): zero native GS in `fast`.** Everything above made the native GS's thread
cheaper; none of it took the native GS out of `fast`. The owner: "I want ZERO native gs in the fast renderer by any
means necessary", the whole job, however long. The plan for it is "The PC renderer: `fast` without the native GS"
(below); it replaces "Next" and "The earlier order" further down.

**Since (2026-10-29, later still): the world's snapshot, steps 1-3 of the next item below** ("The world's snapshot",
below; `KF4_PC_SNAPSHOT`). A picture between makes each world draw the frame kept whole from the frame's snapshot: its
calls put back at its place in the chain, the registers and CLUT left as its writes left them without the writes, its
record made from the kept draw (no fetch, no batches walked). The kept calls' table count is replaced by an exact check
of what each call reads (the table's answer for its texture). At map 000 169,380 of 172,537 kept draws are made so, the
pictures byte for byte as with the switch off and as the build before. **It took less than hoped**: a recording 4.37-4.47
ms against 4.62-4.65 at 000 (-4%), 4.49-4.55 against 4.81-4.94 at 009 (-7%); the world's draws were not most of a
recording any more. Sampled at 000 (106 samples in recordings, `eu-stack`): the snapshot's draws 29% of a recording
(of it the CLUT mirror hashed again after each load about 6%, the table's answer `pcd_table_same` about 7%, the CLUT
loads about 4%), the uploads asked for again (`replay_upload`) 29%, the rest the GS's own batches, the 2D and the
walk. So next is the uploads, then the snapshot's own checks made cheaper (exact still: e.g. the CLUT compared by the
loads' sources, the table's answer kept by its pages), then the GS's own batches, one kind at a time.

**Next: the world off the native GS, done properly (the owner's decision, 2026-10-29).** Item 3 below is the next
step, not a trim of the recording: the owner chose it over trimming, and chose that it be done the proper way, no
shortcut or workaround (the kept calls' table check, above, is the example: replaced by a check of what is read, not
removed). The kept calls ("P7, kept") are its snapshot of the world. What it has to solve, in order:
1. **Its place in the chain.** The world's draws are among the GS's own (the shadows, the batches P7 declines, the
   effects it does not take, the 2D), and blending and Z are right only in the chain's order. The snapshot's calls go
   back in at their places among the GS's draws, without the walk working them out.
2. **The GS's state they leave.** The GS's batches after a world draw expect the registers it wrote, and the CLUT loads
   its TEX0 writes start. That state is set as each place left it, without the writes (and the CLUT as loaded then).
3. **The picture's records.** Each draw's moves (`nd_model_flags`, the sky's and objects') are made in the walk now, one
   draw at a time. They are made another way: all at once from the kept draws, or on the GPU.
With those, a picture's world costs the GS's thread nearly nothing, and the walk is left with the 2D, the effects the
GS draws and the uploads. Then item 1's rest (the uploads asked for again: 31% of a recording at 000 before the lock
fix) and the GS's own batches, one kind at a time, toward a renderer apart from the native GS.

**The earlier order** (before the owner's decision):
1. **P5 continued: the recording made cheaper** (part 1's P5 and part 2's step 2a are one piece of work). What a
   recording still costs, from both parts' profiles: native draw's draws as a picture between makes them (74% of a
   recording at map 000), of them the skinned models' batches, each batch's state written and flushed again though
   the frame already has it (28%); the uploads a picture between asks for again (13% at map 000, 27% at map 009); the
   test at t 0 of the batches the frame drew nothing of (15% at map 009); the PC world's flushes. One at a time, each
   measured with `pace_test.py` at 240 and unlocked (several alternating runs: a third of runs lose a second with no
   hitch logged, cause not found) and the recording's time in `--stats`. Since P7 the world's batches are draw calls
   made from meshes, so a recording's world is P7's calls again; at map 000 a recording went from 4.3 to 4.8 ms.
   P7's declined kinds next: shader blending for 0x280 and 0x700 by in-pass copies over the plan's screen places,
   the shadows, the water's ripple, the trails.
2. **Then the GPU**, once the thread has room: first what the 0.5 ms of "pass: untagged" is (full-screen work at the
   start of passes, not identified; cheap to find); then step 4's items 2 and 3 (one scene pass a picture, about
   0.15-0.4 ms; the alpha-tested Z-first draws, about 0.1-0.25 ms).
3. **A picture between without the frame's packets.** P5 as first designed interpolated the world from snapshots. The
   snapshot cannot be made on the game's side ("P2, revised"), but the kept draws are a snapshot made on the GS's
   thread: a picture between drawn from them alone (and the GS's recording for the 2D and effects), not by walking
   the frame's packets again, takes the recording's world cost away rather than trimming it. A large change; to be
   decided with step 1's measures in hand.
4. Step 2b (the pictures between made by the back end), if unlocked past the screen's rate still wants it then; and
   step 5 (stalls outside the renderer).

**The owner's choices** (both parts):
- **The name** is `fast` (`--renderer fast`, the Debug page's choice and `settings.json`'s `display.renderer`).
- **The look** is the original's, as exactly as a PC renderer can have it: the PS2's lighting, fog, colours and
  draw order, the vertex programs as they are. Room is left for a remastered look (the shaders' inputs carry what
  per-pixel light and real fog would need), but nothing of it is the default.
- Step 1.4 (smooth play): a pipeline not made yet in `fast`, the batch left out for a few pictures (proposed) or
  the stall. Undecided.
- Unlocked without VRR is mailbox (no tearing; 2026-10-28); immediate is `--tearing`. Whether fixed rates leave the
  settings page's main list is still open.
- P1 under `modern` too: open (part 1's last section).

# The PC renderer: `fast` without the native GS (begun 2026-10-29)

**The goal.** `--renderer fast` runs no native GS code: no `gsn.cpp`, no VRAM image, no GS render targets, no GS
batches, no GS thread. A renderer of its own (`port/gs/pc/`, "the PC renderer") takes the same input the native GS
takes and draws every picture with the GPU's own pipelines. The native GS stays as `--renderer native` (and `modern`),
the reference; paraLLEl-GS stays as `gs`.

**What it takes in.** The seam is the native GS's C interface (`gsn.h`), which `gs.c`, the presenter and
`frame_interp.c` call. The PC renderer has its own (`pcr.h`) with the same calls; `gs.c` picks one at start
(`port/platform/gs_api.h`: `gsr_*`, the native GS's or the PC renderer's). Its input is what the game sends:
- **GIF packets** (PATH1, 2 and 3): register writes, primitives (sprites, triangles, strips), image transfers.
- **Native draws** (register 0x7B, `RD_ND_*`): the game's 3D as draws with meshes, matrices, lights and fog
  (`port/render`, `game/native_draw`), drawn by the PC renderer's own vertex shaders (P7's `pc_000/280/700.vert`).
- **The vblank's display registers**, and the port's marks (0x7C uploads asked again, 0x7D pictures between).

**What it keeps instead of VRAM.**
- **Surfaces**: every buffer the game draws into (the frame and Z buffers, the reflection's 256 x 256, the frame copies,
  the scrolling textures' targets) is a GPU image, found by its base, width and format; at the renderer's scale, its own
  formats (D32F depth).
- **Textures from their uploads**: every image transfer is kept as a chunk (where it went, its bytes, a hash), and an
  ownership map says which chunk owns each block. A texture is decoded from the chunks that own it (the PS2's texel
  formats are the game's data: decoded with the swizzle functions of `gsn_vram.c`, a format library, the only file of
  the native GS's directory the PC renderer uses), or read from the surface drawn there since.
- **Copies** (local to local) are image copies between surfaces; read-backs (local to host) read a surface.

**How it draws.** Native draws by their records (P7's maths, the material from the registers at the draw). GIF
primitives by one generic pipeline (the GS's colour arithmetic as the scene's shaders have it: texture function, alpha
test, fog, blending by Vulkan's blend where it can, the shader's from a copy where not; DATE by the stencil). The
display registers pick the surface shown; the picture goes to the presenter as the native GS's does.

**The order** (each step runs, and is checked against `--renderer native`'s pictures):
1. **The skeleton**: `--renderer pc` (development name; `fast` is switched to it at the end), the device, its thread,
   the vblank, the presenter, a picture (cleared surfaces), `gs_api.h` in `gs.c`, the presenter and frame_interp.c.
2. **The 2D**: register state, sprites and triangles from GIF packets, uploads and the texture store, surfaces, the
   scanout. The title, menus, HUD, messages and movies show.
3. **The 3D**: native draws (0x000, 0x280, 0x700) with depth, fog, alpha test, blending, layers.
4. **The rest of the play**: shadows (0x5A0, DATE as the stencil), the PATH2 strips (the trail, the water), the
   reflection target, the frame copies, the scrolling textures, the screen passes.
5. **Pictures between**: each picture drawn from the frame's draws with the picture's camera and moves, on the GPU.
6. **The rest of the interface**: save states, captures, `--dump-frames`, `--stats`, the overlay's settings.
7. **`fast` switched to it**, the checks (pictures against native at every spot and the map tour's maps, sanitizers,
   speed), the old `fast` kept as `--renderer modern`.

## Step 2 built (2026-10-29): the PC renderer draws the whole game from its GIF packets

`--renderer pc` draws every picture the game sends as GIF packets, with none of the native GS's code
(`port/gs/pc/pcr_canvas.cpp`, shaders in `port/gs/pc/shaders/`, built by `tools/build_shaders.py` into
`pcr_shaders.h`). Native draws are turned off under it for now (the game's VU1 packets carry the 3D), so the picture is
whole; drawing the native draws themselves is step 3.

- **Surfaces**: a GPU image for each buffer drawn into (base page, width, format), as tall as what is drawn in it. A
  frame whose base is inside a surface of its width is that surface at an offset (the game's 128-wide targets at 1d2
  and 1d4 are one buffer a page row apart); a surface made takes in those of its width it overlaps. A new surface
  starts as the uploads left its pages.
- **Textures**: the texture store's (uploads, exact, `pcr_textures_test`), or a copy of the surface drawn there.
  Local copies of uploads' pixels are uploads of what they write (the scrolling textures); read-backs (local to
  host) are made from the uploads or the surface: the game reads its effect atlas back and uploads it again, so a
  read-back of zeroes lost the torches' flames.
- **The GS's pixel** in `pcr_prim.frag`: texture read (wrap modes, filters, the mipmap level from Q, CLUT, TEXA),
  texture function, fog, alpha test with AFAIL's passes, DATE (from a copy of the destination), FBA; ALPHA by
  Vulkan's blend (the colour premultiplied in the shader where it needs it; As above 128 is clamped: counted).
- **Pictures between** at `--frame-rate` 60-240: frame_interp.c's own way (the frame's packets sent again, moved),
  each drawn and scanned out.
- **Measured against `--renderer native --native-draw off`** (title, menus, map 000 at vblanks 300-2700): 29 to
  4,263 of 286,720 pixels differ by more than 8 (blend rounding, the 2D's edges); the torches, the HUD's gauge, the
  menus and the scrolling textures as native. `KF4_PC_CENSUS=1` counts what the game sends (targets, tests, blends,
  textures, transfers) for `--stats`.
- **Save states** (built after): the GS's 4 MB first (the uploads' chunks in order, the colour surfaces' pixels over
  the pages drawn last), then the registers, Q and the CLUT's mirror; taken back as one upload of the whole memory.
  States of the native GS load into the PC renderer and the PC renderer's into the native GS by their first 4 MB
  (gs.c's take_other_vram). Each of the three, loaded at 2600 at map 000, gives at 2700 the picture of the run that
  went on (0 pixels differ). Z buffers are not kept (the game draws its Z anew each frame).
- **Not yet**: native draws (step 3), MSAA, `--native-colour`,
  anisotropic filtering, captures, the overlay's settings; performance not measured yet.

# Part 1: the PC renderer (P1-P6)

A real PC renderer for King's Field IV's world: the map, the map objects, the items, the characters and the arm
drawn by our own Vulkan renderer from the game's data and state, the way a PC game draws its world, instead of
the PS2's GS emulated draw by draw. The GS path keeps what is 2D or over the whole screen (the HUD, the menus, the
text, the fades and the screen passes) and, until they are ported, the effects. `--renderer modern` stays the
reference (the PS2's pictures, checked byte for byte); `fast` is checked against it by how much it differs, not
whether, and is free to differ where that is the point (filtering, depth, anti-aliasing, frame rate).

**Status as written when the plan was closed out** (October 2026; corrected at the top: the native GS's thread, which
"limit no spot measured" then, limits map 000 above about 200 pictures a second at the owner's later settings):
the plan, agreed with the owner (October 2026); P1 built, P2 built (the map's records, objects, items and gold,
opaque, alpha-tested and blended), P3's first two steps built (the skinned parts' first pass drawn once, alphas a
little above 1.0 by Vulkan's blending): at 170 Hz `fast` keeps the owner's settings at 168-170 pictures a second at
maps 000 and 009, where P1 kept 84 and 118 (below, "In real time"). The steps since P2's first were decided overnight
for the owner's review. P4-P6: "Where P4-P6 stand" below; P5's first step and P3's third built (the owner suspended
the rule of agreeing the interface first while the plan is finished). **Where the plan ends** (October 2026): at 170 Hz
every spot keeps 168-170 pictures a second; at 240 maps 003, 004, 009, 013, 017 and 030 keep 239-240, map 000 236 (GPU
bound: 239 at 2x with aniso 1). What is left is not a phase's: the scene's own draws, the Z-first draws of As up to
2.0 (below, "What still has a Z-first draw"), the GS's 2D and screen passes; and on the GS's thread the uploads a
picture between asks for again (P5, "What a recording still costs"), which limit no spot measured.

## Why (measured October 2026, branch `p8/gpu-plan`)

The owner's machine (RX 9070 XT, RADV), the owner's settings (`tools/pace_test.py`'s `OWNER`: modern, 4x with
native widescreen, aniso 16, cut-out AA), the pond (map 009), headless at 240, `KF4_GSN_GPU_TIME=2` (the marks
wait for the work before them: read the kinds as shares):

| | GPU a picture | the scene's colour draws | its alpha-tested Z-first draws |
|---|---|---|---|
| the owner's settings | 5.58 ms | 2.57 | 1.29 |
| aniso 1 | 4.79 ms | 1.95 | 1.10 |
| 2x, aniso 16 | 3.34 ms | 1.24 | 1.01 |
| 1x, aniso 1 | 2.16 ms | 0.52 | 0.74 |

The budget is 4.17 ms a picture at 240 and 5.88 at the owner's 170 Hz. About 550 Vulkan draws and 8,000
triangles a picture. Two costs:

1. **A cost a pixel**: `gs_frag.h` is the GS's texture unit bit for bit (raw texels, each read wrapped and put
   through the CLUT, bilinear four such reads, trilinear eight, `--native-aniso 16` up to sixteen trilinear
   reads). The colour draws scale with the pixels and aniso 16 adds 0.8 ms.
2. **A cost that does not scale**: 2.16 ms at 640 x 448. The ties' Z-first draws, shader blending's copies, the
   turns between the scene and the GS's targets, the render passes: the GS's way of drawing, with the PS2's
   order, state and Z for each batch.

A PC renderer has neither: textures sampled by the hardware, and a world drawn by material with one depth buffer.

## What the game gives us (the decomp)

- **The frame** (`GameLoop_DrawFrame`, `src/game/core/game_loop.c`) draws the world as one block, then the rest:
  `Map_DrawRecords`, `MapObject_DrawAll`, `Entity_DrawPoolModels` (the shadows), `Entity_DrawAll`,
  `MapItem_DrawAll`, `ItemEffect_Draw`, `Gold_Draw`, `Map_DrawLateRecords`, `Effect_DrawAll`, `DrawList_Draw(0)`,
  then with the camera before the eye (`GameLoop_SetCameraMatrices(0, 2)`) `DrawList_Draw(1)` and `(2)` (the arm,
  what is held), and `MapItem_DrawPicked`. `map_draw_state.mode` 1 draws the world mirrored first (the camera's
  mode 1) under a tint; mode 2 draws it into an offscreen target first (the water's reflection). The wobble, the
  tints, the brightness, the fades and the HUD come after the block.
- **The map** is `map_slots[2]` (the map and the one streaming beside it), each with its records already culled by
  the game into `record_draw_lists[map]` and `late_record_draw_lists[map]`. A record is a `ModelInstance`: its
  parts (a tree of `ModelPart`, each a matrix and its data blocks), a light block (two matrices) and a fog block
  from `map_draw_state`, point lights from the linked points that touch it (`LinkedPoint_FindHitsForMapDraw`), its
  tint, fade, alpha and texture (`texture_data`, sent by `TextureTable_QueueEnabled` / `Map_QueueSlotPacket`).
- **The geometry** is the PS2's own format in the files: each part's data is VU1 data blocks (UNPACKs and the
  MSCALs that draw them). `port/render/render.cpp` already parses them into meshes, cached per block.
- **Native draw's adapters** (`game/native_draw/`) already sit at those call sites and turn every draw into an
  `RdDraw` (`port/include/port/render.h`): a stable identity (kind, instance, part, record), its meshes, its
  camera, its matrices, lights, fog and point lights, as VU1's fixed block has them. That is a scene description
  of the frame, made by the game's own code, one draw at a time.
- **The textures** reach the GS as uploads of the game's chunks, and native draw's page-ownership table
  (`gsn_standalone.inc`) already decodes each texture once from the uploads that own it, with its CLUT. The table
  stays the source: it sees every writer (the project tried hooking the loaders and kept the table for that).

## The design

```
game thread (30 a second)                       renderer (each picture)
GameLoop_DrawFrame                              
  world block ── RdDraws (as now) ──► the frame's snapshot ──► PC renderer: views, instances by material,
  (no GS packets for the world)          (views, draws, ids)       hardware textures, one depth buffer
  RD_ND_WORLD mark in the chain ─────────────────────────────► the world composited here, the GS's depth
  HUD, passes, 2D ── GS packets (as now) ─────────────────────► native GS, drawn over the world
```

- **The snapshot.** With `--renderer fast`, the adapters keep making `RdDraw`s but do not queue them in the chain;
  the frame's draws, grouped by the view they are drawn in (the world, the mirror, the offscreen reflection, before
  the eye), go to the renderer whole at the end of the world block. The chain gets one mark there
  (`RD_ND_WORLD`): where the world's picture and depth are put into the GS's frame, so that everything after
  (effects not ported yet, the passes that read the frame, the HUD) draws over it as before.
- **Meshes, persistent.** A mesh (a parsed block) is put on the GPU once and kept while the game keeps its block:
  the map's when the map loads, a model's when it is first drawn. Vertices in the model's space, as the files have
  them; the instance's matrices place them.
- **Materials.** A batch's texture (from the table, expanded to RGBA8 with its mip levels through the CLUT in use,
  kept by texture and CLUT), its alpha test, its blend, whether it writes depth, its fog. Opaque and alpha-tested
  batches drawn by material; blended ones in the game's order within their view.
- **The original's look.** The vertex programs of native draw (`nd_000.vert`, `nd_280.vert`: VU1's lighting,
  the point lights, the fog, the skinning) are the shaders' first stage, and what comes after the texture read
  (TFX, the alpha test, the GS's blend arithmetic where Vulkan's differs) is `gs_frag.h`'s, so a surface is lit,
  fogged and coloured as on the PS2. What differs is only what a PC renderer must or should: the texture
  filtering's precision, the level of detail by the pixel's footprint, depth without the GS's rounding, and
  pictures between. The goal is pictures a player cannot tell from `modern`'s at a glance; the spots' differences
  are reported per phase and each one explained.
- **Room for a remastered look.** The vertex stage passes on what per-pixel lighting would need (the normal and
  the world position beside the lit colour), and the map's light and fog blocks are kept per instance, so a
  remastered look can be a later shader choice without touching the snapshot or the caches.
- **Depth.** One reversed-Z float depth buffer for the world. Coplanar layers (decals, signs, the lava's edges)
  by their order within their record and a depth bias, not the ties' Z-first draws.
- **Pictures between, from snapshots.** The renderer keeps the last two snapshots and draws each picture at its
  time between them: the camera and every instance's matrices interpolated, paired by identity (and by the
  game's own instance where the identity is the model's address: the look-alikes that swap now). No recording and
  no replay for the world; the GS's recording keeps doing the 2D and effects.
- **What stays the GS's**: the screen passes, the HUD, the menus, the text, the movies; the effects (program 0x700)
  until P6. They draw into the GS's frame after the world, with the world's depth given to the GS's Z buffer for
  the effects that test it.

## The phases

Each is playable, measured with the profiler (Shift+P) and `KF4_GSN_GPU_TIME`, and compared with `modern` at the
pond, the fortress, the canyon, map 000 and under water (the picture difference reported per spot). `modern`, the
PS2 path and their checks (RENDERER.md) are not touched.

- **P1: hardware textures.** The texture cache the renderer samples: a table texture expanded on the GPU into
  RGBA8 with its mips, kept by (texture, CLUT), sampled with Vulkan's filtering and anisotropy. Built first and
  usable alone: under `modern` as a switch (`--native-sampler hardware`, the pictures differ in filtering only)
  it takes the per-pixel cost out of today's scene at once, and the PC renderer needs it anyway.
  **Built** (`port/gs/native/gsn_rgba.inc`; `gs_frag.h`'s `GSN_HW_SAMPLER`, built as `gs_hw.frag`, `sa_sample_hw.frag`,
  `gs_in_hw.frag`, `gs_in_ms_hw.frag`; `--native-sampler hardware`, which `--renderer fast` takes). A table texture
  (`sa_table_texture`) keeps its raw texels on the CPU; the first batch that draws it with a given CLUT (or TEXA, for
  CT24 and CT16) expands them as `texel_f` decodes them into an RGBA8 image with its levels, uploaded in the stream as
  the raw image was, and kept by (the decode's serial, the CLUT or TEXA). The scene's program-0x000 batches drawn by
  `sa_draw_batch` read it at binding 6 with a stock sampler: TEX1's MMAG nearest, nearest; linear between levels (MMIN 3
  or 5), the geometry filter with `--native-aniso`'s anisotropy; else bilinear; CLAMP's REPEAT or CLAMP. The level
  comes from the pixel's footprint (no scale's bias; `--native-lod full` still level 0). The GS's sampling is kept for
  a render target or VRAM's texture, a region clamp or repeat, S and T wrapped differently, and the skinned groups
  (`sa_group_state`: their own bindings; not yet measured to be worth it). `--stats`: `native sampler hardware`.

  **Measured** (the pond at the owner's settings, as "Why"): 5.58 -> **3.45 ms** a picture; the scene's colour draws
  2.57 -> 0.73, the alpha-tested Z-first draws 1.29 -> 0.66. 534,274 batches read RGBA8 textures, 134 made (15 MB),
  4,498 kept the GS's sampling (not from the table). The pictures against `modern`'s: a third of the pixels differ, by
  0.43 levels on average and none by more than 16 (the GPU's bilinear has 8 bits of fraction to the GS's 4; the level by
  the footprint); side by side they cannot be told apart.
- **P2: the snapshot and the map.** The `RD_ND_WORLD` mark and the frame's snapshot; the PC renderer draws the map
  records (program 0x000, opaque and alpha-tested) into its own colour and depth, composited at the mark; every
  other world draw still goes to the GS that frame (the turns as now) so the game stays whole while the renderer
  grows.

  **Built, first step** (`port/gs/native/gsn_pc_world.inc`; the snapshot made on the native GS's thread, below, "P2,
  revised"). With `--renderer fast` a scene batch whose corners are all a map record's (`RD_KIND_MAP_RECORD`, program
  0x000, not the far sky) is kept by `sa_draw_batch` once its state is worked out, when it is opaque or alpha-tested (no
  ABE, or ABE that leaves the source: (Cs - Cd) As + Cd with As 0x80 at every pixel, which the map's records set on
  their opaque surfaces), one pass, one run, no shader blending, read by P1's sampler, into the scene's frame (not a
  named target). Kept with its state (`PcKey`: the pushes, masks, scissor, the RGBA8 view and sampler, the records;
  the image and buffers held), and drawn when anything else comes: the same places that draw a skinned model's kept
  parts (`sa_group_draw` now draws both; one of the two is pending at a time), and before any batch the scene draws
  itself. A kept material is one draw: its colour, Z written, GEQUAL, exact (no Z-first draw, no tie slack).
  The corners of a batch are drawn after the draw that made them ends (`sa_flush`): what decides is whether the
  pending corners came from a taken draw (`Sa::pending_pc`, `batch_pc`), not the draw being met. Switches for
  measuring: `KF4_PC_ORDER=1` merges a batch into the first material of the same state, not only the last;
  `KF4_PC_TIES=1` keeps the batch's own draws (its Z first, its colour within the slack). `--stats`: `pc world`.

  **Measured** (October 2026; the owner's settings as "Why", headless at 240, the teleport spots `--teleport
  1900:N:0` after `tests/newgame.pad`, picture 3799, `KF4_GSN_GPU_TIME=2`; GPU ms a picture, two runs each; the
  pictures against P1's, which is `modern` with `--native-sampler hardware`):

  | map | P1 | fast | kept a flush | pixels differing from P1 (worst) |
  |---|---|---|---|---|
  | 000 | 6.43, 6.93 | 6.76, 6.83 | 2.4 | 98 (26) |
  | 003 | 5.23, 5.39 | 5.01, 5.03 | 4.1 | 272 (81: the brazier's glow on the wall behind it) |
  | 009 | 5.96, 6.00 | 5.45, 5.85 | 1.9 | 186 (10) |
  | 013 | 3.71, 3.99 | 3.50, 3.67 | 2.9 | 0 |

  So: about 0.2-0.4 ms a picture where the map is much of the scene, nothing at map 000, and the pictures as P1's but
  for a few hundred pixels where a layer meets a surface. The saving is the kept batches' Z-first draws, not fewer
  draws: a flush keeps 2-4 batches, each its own material (`KF4_PC_ORDER=1` merged 11% more at map 009, measured before the
  ABE rule above). The map's records alternate with what must stay in order between them: their blended layers (58% of
  the taken batches at map 009, ABE with As not 0x80) and the objects of each map slot. What still costs at these
  spots is the rest of the world (the skinned groups, the objects', items' and entities' Z-first draws, the blend
  copies), which P3 takes; and the layers, which need the depth-bias rule below in place of the scene's ties before
  they can be kept. Not yet measured at the owner's quick-save spots (the pond, the fortress, the canyon, under water).
  `modern`'s pictures byte for byte as before (maps 000 and 003; a first run of map 000 before the change differed from
  a second by 64 pixels on its own); ctest 6 of 6, vu1_test 0 of 90 failed; ASan + UBSan (clang) on `fast` at map 003:
  nothing in the changed code. No unit test: the keeping is the scene's state on the GS's thread, checked by the pictures.
  **Built, second step: the blended batches, and the objects, items and gold** (October 2026, decided overnight, for
  the owner's review). Measuring why the taken batches were not kept (map 009) found one case only: a blend that does
  not leave the source, nearly all of it the cut-outs' (alpha test As > 0, (Cs - Cd) As + Cd, Z written: the branches,
  the braziers, the fences; the scene's "Z-first atest", 1.2 ms of the pond-style spot). They are kept now when Vulkan's
  blending does the GS's (no shader blending), drawn whole in one draw: GPU blending and the Z test follow the
  triangles' order, as the GS does, so the scene's Z-first draw and its runs are not needed. The key carries the blend
  (`PcKey`'s `blend_on`, the factors, FIX). And the take is every program-0x000 kind of the world block, not only the
  records: `pc_world_kind`, the map's records, its objects, the items and the gold (`KF4_PC_KINDS`, a mask of
  `RD_KIND_*` bits, for tests); the entities, the arm and the shadows are other programs.

  **The layers' depth.** A blended batch keeps its tie slack (the scene's colour pass's, the two world units of
  `sa_slack` for a layer) and writes Z biased by it. Three other ways were measured and dropped:
  - its Z exact (no bias): map 000's rock over the tower's top (a layer a unit off the bricks, the case `sa_slack`'s
    world units were made for) lost a triangle of itself, 5,700 pixels;
  - the tie's one Z only, not the world units: 4.6% of map 000 differed (the layers far off);
  - the scene's own draws (its Z exact first, then its colour within the slack), whole or run by run: no closer to
    P1, and run by run a draw a triangle (5.2 million at map 000), slower than P1.
  What the bias written costs: a line a pixel wide at the edges of a cut-out in front of what is drawn after it and
  tested against its Z (the braziers' legs against their fire's glow on maps 000 and 003, the edges of map 013's
  roof beams): the line where the slack's Z, not the GS's, decides.

  **Measured** (as the first step's table: the owner's settings, headless at 240, `--teleport 1900:N:0` after
  `tests/newgame.pad`, picture 3799, `KF4_GSN_GPU_TIME=2`; one run each, against P1, `modern --native-sampler
  hardware`):

  | map | P1 | fast, first step | fast | pixels differing from P1 (worst) |
  |---|---|---|---|---|
  | 000 | 6.68 | 6.76, 6.83 | **4.86** | 1,916 (219: the brazier's legs, above) |
  | 003 | 4.95 | 5.01, 5.03 | **3.79** | 8,419 (215: the braziers' legs and rims) |
  | 009 | 5.64 | 5.45, 5.85 | **4.88** | 2,129 (17: specks in the fire) |
  | 013 | 3.84 | 3.50, 3.67 | **3.19** | 1,468 (45: the roof beams' edges) |

  So 0.65-1.8 ms a picture under P1: the alpha-tested Z-first draws 1.23 -> 0.77 ms at map 009, the scene's colour
  draws 1.09 -> 0.90. A flush still keeps few materials (batches kept 62,041 and materials 59,070 at map 009: merging is
  rare while the chain's order is kept; `KF4_PC_ORDER=1` is the switch for trying more), so what saves is the Z-first
  draws and runs left out, not fewer draws. `modern`'s pictures byte for byte as before (maps 000 and 003, against the
  commit before); ctest 6 of 6, vu1_test 0 of 90 failed; ASan + UBSan (clang) on `fast` at map 003: the sound driver's
  and the decomp's known reports only, nothing in the changed code. No unit test (the keeping is the scene's state on
  the GS's thread, checked by the pictures). No clang-tidy: the project has no configuration for it.
- **P3: the whole world.** Blended and layered records, map objects, items, gold, entities (0x280, skinned), the
  sky, the shadows, the arm and what is held (the eye's view), the picked item. The GS then draws no world. (The
  blended and layered records, the objects, the items and the gold: P2's second step, above.)
  **Built, first step: a skinned part's first pass drawn once** (October 2026, decided overnight, for the owner's
  review). What the scene still draws itself at map 009, counted by triangle: the skinned models' (program 0x280: the
  entities, the arm) 8.6 million with a Z-first draw, far more than the rest (the shadows 3.8 million, by the stencil;
  the reflection's water 0.6 million; the 0x000 batches blended in the shader 0.2 million). The skinned groups (pf/skin)
  draw a group's first passes twice, their colour with no Z written and then the same draw exactly with Z. A part's
  slack is 0 (`skin_eps`), and every pixel passes the alpha test (role 1's `covers`), so the second draw writes, last,
  every pixel the first did: the second alone leaves the same colour and Z. With `--renderer fast`
  `sa_group_first_draw` leaves the first out when the two draws are the same but for the Z write
  (`KF4_PC_GROUP_ONE=0`: both). The pictures byte for byte as with both, at maps 000, 003, 009, 013 and 017; GPU ms
  a picture (the owner's settings, as P2's table):

  | map | both | one |
  |---|---|---|
  | 000 | 4.84 | **4.53** |
  | 003 | 3.80 | **3.59** |
  | 009 | 4.89 | **4.66** (the skinned groups 0.63 -> 0.40) |
  | 013 | 3.17 | **2.92** |
  | 017 | 2.74 | **2.63** |

  It would give `modern` the same pictures for the same saving; it is left to `fast`, as `modern` is not touched
  (the owner's call whether it should have it). `modern` byte for byte as before (map 009); vu1_test 0 of 90 failed;
  ASan + UBSan (clang) on `fast` at map 009: the known reports, and one in native draw's water (a signed overflow
  in the texture coordinates, `gsn_nd_water.inc`), fixed in its own commit.
  **Built, second step: alphas a little above 1.0 blended by Vulkan** (October 2026, decided overnight, for the
  owner's review). What the scene still drew with a Z-first draw at map 009 after the first step was nearly all in the
  pictures between (the skinned models are drawn only there at 240: the frame's own picture keeps them for the
  pictures between): the creatures' parts whose textures have alphas up to 0x88 (their CLUTs' 0x84 and 0x88), so As
  above 0x80 and (Cs - Cd) As + Cd blended in the shader (`as_over`), each run of a part a copy of the scene and two
  barriers: "scene blend copies between" 0.65 ms, and their runs' Z-first draws. `modern` pays the same (0.49 ms).
  With `--renderer fast` `pc_world_alpha_over` lets Vulkan's blending take As up to 0x90: it clamps the factor to 1.0,
  so such a pixel comes out Cs, not Cs + (Cs - Cd)(As - 1.0), at most an eighth of (Cs - Cd) off (`KF4_PC_ALPHA_OVER`,
  128 for the GS's rule). At the creatures (009:2, `--teleport 1900:9:2 --invincible`, two of them close): 3% of the
  pixels differ, by 3 levels at most (their shirts and hair), and

  | spot | P1 | fast, before | fast |
  |---|---|---|---|
  | 009:2, the creatures | 5.46 | 4.54 | **3.15** |
  | 000 | 6.68 | 4.53 | **4.52** |
  | 003 | 4.95 | 3.59 | **3.58** |
  | 009 | 5.64 | 4.66 | **3.64** (copies between 0.65 -> 0.04, Z-first atest 0.77 -> 0.20) |
  | 013 | 3.84 | 2.92 | **3.00** |
  | 017 | 3.41 | 2.63 | **2.53** |

  (GPU ms a picture, the owner's settings, as P2's table; map 009's place 0 draws the creatures in the pictures
  between though none is in view.) The pictures against P1's at places 0 as P2's second step's, 017 660 pixels
  (worst 37). `modern` byte for byte as before (009:2, its frame and a picture between); ASan + UBSan (clang) on
  `fast` at 009:2: the known reports only.
  **In real time** (`tools/pace_test.py --maps 0,9 --hz 170 --mode pace`, the owner's settings at 170 Hz, one run
  each, October 2026, after P3's second step):

  | map | P1 (`-- --native-sampler hardware`) | fast (`-- --renderer fast`) |
  |---|---|---|
  | 000 | 84 pictures a second (lowest second 58), the game slowed to 41 vblanks a second | **168** (lowest 161), 60 |
  | 009 | 118 (lowest 98), 42 vblanks a second | **170** (lowest 168), 60 |

  The back end's thread (the Vulkan calls, `gsn_backend.inc`) does much less: 10.5 -> 3.4 s of a 3,800-vblank run at
  map 000, 4.7 -> 2.6 s at map 009; the native GS's thread's time a picture between (drawn from the packets) is as
  before (5.8 ms at map 009, `--fast`), which is P5's.
  **Built, third step: the skinned groups read P1's textures** (October 2026). P1 left the skinned groups
  (`sa_group_state`, pf/skin) on the GS's sampling: their key bound the raw texture and the CLUT. At map 000 at 240 the
  GPU was the limit (2x and aniso 1 kept 239 pictures a second where the owner's settings kept 233), and the groups were
  the second largest kind. With `--renderer fast` a group's key carries the RGBA8 view and sampler `sa_draw_batch` made
  for the batch (`SaGroupKey::rgba`, the image held by the pass), drawn with `programs_hw` (the shines blended in the
  shader with `programs_in_hw`); `KF4_PC_GROUP_HW=0`: the GS's sampling. The groups' GPU time (headless at 240,
  `KF4_GSN_GPU_TIME=2`): map 000 0.78 -> 0.53 ms, 009:2 0.56 -> 0.29; a picture 4.20 -> 3.96 and 3.10 -> 2.82. The
  pictures differ in filtering only, as P1's: 0.7% of map 000's pixels by 3 levels at most, 4.5% of 009:2's (the creatures)
  by 9 at most. `modern` byte for byte as before (009:2, frame and pictures between); ASan + UBSan (clang) on `fast` at
  009:2: the known reports only. At 240 in real time map 000 233 -> 236.5 pictures a second, 009 239.

  **Measured and left: the effects, and the skinned plans** (October 2026, overnight). Two things tried after the
  second step, neither kept on:
  - the effects (program 0x700) taken by the PC world (`pc_world_takes`; a key now carries its corners' vertex program,
    `pc_corners_vs`): the pictures the same, the GPU time the same within the runs' noise (map 009 3.76-3.78 against
    3.59-3.85 without; 000 4.50, 013 2.94): an effect has no Z-first draw to leave out and is a material of its own. Off by
    default; `KF4_PC_KINDS=0x9E` takes them, for P6.
  - the skinned plans' cache four plans a batch instead of two (the world's and the reflection's, two models of a mesh):
    at 009:2 the plans found 7.37 million vertices against 7.36 million, so the plans planned again are new ones, not
    two models taking each other's. Dropped.

  **And one guard:** what the PC world keeps is drawn before a batch for another frame target binds that target
  (`pc_world_flush_for`, at `sa_draw_batch`'s start, as the skinned groups have it): the kept materials draw into the
  target bound when they are flushed. It never fired at the spots (`--stats`: `pc world: ... flushes (N for another
  target's batch)`), so no picture changed, but nothing kept it from coming. And the sweep: `--renderer fast` at every
  map 000-063 (`--teleport 1900:N:0 --invincible`, 400 vblanks after, the owner's settings): no crash, no error.
- **P4: reflections.** The mirror (mode 1) and the offscreen reflection (mode 2) as views of the same scene; the
  rippled water (`RD_PROGRAM_RIPPLE`), the scrolling textures.
- **P5: pictures between from snapshots.** The world's pictures between by interpolation; the recording keeps
  only the GS's part.
- **P6: effects.** Program 0x700's draws as instanced particles; then the GS keeps only the 2D and the passes.

## Where P4-P6 stand (October 2026, overnight, for the owner)

P2 and P3's steps grew the PC renderer inside the native GS's scene (the materials kept and drawn as the chain meets
them, P2 revised), not as the separate snapshot renderer the phases above were written for. With that, the speed the
phases were for is mostly here already: at 170 Hz every spot measured keeps 168-170 pictures a second (maps 000, 003,
004, 009, 013, 017, 030; map 021 draws no pictures between with P1 either). What is left of each phase, measured:

- **P4, reflections.** At every place measured (009:0-6 and the spots) the reflection costs nothing on the GPU ("refl"
  kinds 0.000-0.003 ms): the scene draws it into its named target (T3) and the pictures between keep the frame's
  (`sat_kept`: about 500,000 batches a run left out). What remains is the water's surface (`RD_KIND_WATER`, the ripple
  program, its own bindings: `ndw_batch`), part of 0.2 ms of Z-first draws at map 009. Not taken by the PC world: little
  to win at the spots.
- **P5, pictures between from snapshots.** The one large cost left is the native GS's thread: about 5.8 ms of CPU a
  picture between at map 009 (`--fast`, drawn from the packets), the same with P1 and `fast`. Its profile (perf, map
  009, the GS's thread 54% of the program's samples) is flat: memmove 6% (most of it the uploads' data copied, 1.8%),
  the skinned plans 5.4%, VRAM reads 3.9%, `sa_draw_batch` 2.4%, the native plans 2.3%, then many under 2%: the GIF
  front end, the uploads (9.3 GB a run, 168,000 asked for again by the pictures between), the batches' work. No one
  change takes much of it. **First step built**: the draws the PC world takes are drawn again from the frame's kept
  batches (below, "P5, built"), 11-23% of a picture between.
- **P6, effects.** Taken by the PC world, the effects (program 0x700) changed no picture and saved nothing on the GPU
  (P3's "Measured and left", above; `KF4_PC_KINDS=0x9E`). Instanced particles would save the GS thread's work on them
  (part of P5's cost), not the GPU's: with P5.

**What still has a Z-first draw** (map 000, the GPU's 0.76 ms of "scene Z-first atest", counted where the scene draws a
batch itself, after the PC world and the skinned groups took theirs): 2.4 million triangles of a run are 0x000 batches
blended in the shader because their As goes up to 2.0 (textures' alphas to 0xFF, (Cs - Cd) As + Cd), and 0.3 million
the PS2 path's in pictures between. Vulkan's blending cannot do As above 1.0 exactly (a UNORM target clamps the factors
and the output to 1.0, and one blend equation cannot add Cd (1 - As) below 1.0 and take Cd (As - 1) away above it), so
these stay the shader's, as in P1. (Taken first for the semi-transparent creatures: the blended 0x280 batches counted
there were the skinned groups' shines, whose Z draws the groups leave out already; tried and dropped.)

### P5, built: a taken draw's kept batches drawn again in its pictures between (first step)

(October 2026; the owner suspended the rule of agreeing the interface first while the plan is finished.) The frame's
first picture between walks its packets again (rc/own, `nd_prep_*`), and a draw the PC world takes was worked out
again there: its corners copied, its scene batch's state, texture, alphas and blending, to come to the frame's key. Now
(`gsn_pc_world.inc`, after the P2 code):

- **The frame keeps**, for each draw the PC world took (by its place among the frame's native draws, as `nd_prep` keeps
  its records) what `pc_world_keep` was given for each of its batches (`PcKept`): the key, the texture, the context of
  its FRAME and ZBUF, the rows, and every triangle of the batch in order, its slope in its alpha as `sa_draw_batch` worked
  it out (`sa_slope`, now a function), those left out at the scissor marked. A batch it drew nothing of (every
  triangle outside the scissor, ZTST NEVER, an empty scissor) is kept as that, without a key. A draw is whole when every
  batch `sa_draw_batch` met ended in one of these.
- **A picture's draw** at that place, with the same value and its record taken (`nd_prep_take_const`), whole and in
  the same arena (`pc_between_takes`): `nd_draw` makes its record (the picture's moves) and writes the batches'
  registers as before, but plans no corners; `pc_between_draw` gives each kept batch to `pc_world_keep` with the
  picture's record in its corners, into the picture's own FRAME and ZBUF (the buffer shown, not the frame's), with
  `sa_flush`'s and `sa_draw_batch`'s work around a kept batch (the reflection kept, the group's parts, the target's
  flush, `sa_acquire`, the rows). The triangles the frame left out at the scissor are drawn too: the GPU clips them
  (a picture's `sa_draw_batch` keeps one in view at t 0; testing them cost a quarter of the picture's time, drawing
  them nothing measurable on the GPU). A batch with no key and a triangle in view at t 0 (`nd_box_at_0`, its work now
  `nd_box_at_0_of` for any corners) makes the draw worked out as before.
- **A draw's batches end with it** while frames are kept for their pictures between (`pc_between_cuts`, frame and
  picture alike), so that each kept batch is one draw's. That changes the frame's batches (a batch is one draw's
  alphas, so opaque where a longer one was blended): at map 000 70,800 pixels differ from before, 95 by more than 16
  levels (a brazier's rim, up to 219), at map 003 416 (the braziers' rims), none at 009:2, 013 or 017; at 30 nothing is
  kept and nothing changes. `KF4_PC_BETWEEN=0`: neither, `fast` as before byte for byte.
- `--stats`: `pc world, pictures between`: the draws drawn from the frame's kept batches, and those worked out again
  (not every batch kept: a batch the PC world did not keep; a triangle of a batch with no key in view at t 0).

**Measured** (the owner's settings, `--fast` at 240, `--teleport 1900:N:P` after `tests/newgame.pad`, picture 3799; a
picture between drawn from the packets, from `--stats`):

| spot | before | P5's first step | GPU a picture |
|---|---|---|---|
| 000 | 5.67 ms | **4.46** | 3.64 -> 3.42 |
| 003 | 2.85 | **2.19** | 2.08 -> 2.07 |
| 009 | 5.61 | **5.00** | 2.96 -> 2.95 |
| 009:2, the creatures | 3.07 | **2.68** | 2.25 -> 2.23 |
| 013 | 2.81 | **2.36** | 2.11 -> 2.08 |
| 017 | 1.87 | **1.63** | 1.50 -> 1.50 |

Real time at 240 (`tools/pace_test.py --maps 0,9 --hz 240 --mode pace,cost -- --renderer fast`, one run each):

| map | pictures a second (lowest second) | a recording | the GS's thread a game frame |
|---|---|---|---|
| 000 | 197 (187) -> **231 (224)** | 6.35 -> 4.81 ms | 22.2 -> 20.1 ms |
| 009 | 240 (237) -> 240 (237) | 5.87 -> 5.22 ms | 22.1 -> 21.7 ms |

At maps 009, 013, 017 and 009:2 every picture (the frame's and `_250`, `_500`, `_750`) is byte for byte the step's
without the replay (`KF4_PC_BETWEEN=0` at the same commit: map 009, 000), and byte for byte before at 013, 017, 009:2.
`modern` byte for byte as before at map 009 (frames and pictures between); at map 000 one picture between differed by 64
pixels, as two runs of map 000 already did. ctest 6 of 6, vu1_test 0 of 90 failed, boot_test PASS, frame_test
`--renderer native` and `fast` PASS. ASan + UBSan (clang) on `fast` at map 003 with the pictures between (50,052 draws
drawn from the frame's): the sound driver's, the decomp's and the port's known reports only, nothing in the changed
code. No unit test: what is kept is the scene's state on the GS's thread, checked by the pictures. No clang-tidy: the
project has no configuration for it.

**What a recording still costs** (perf, map 009, after the step): the uploads asked for again about 27% (each written
to VRAM again and its blocks given back to the table: the GS's part, which every draw that is not the PC world's
reads), the test at t 0 of the batches the frame drew nothing of about 15% (as before: the old path did the same), the
skinned models' draws (0x280) and their groups, the PC world's flushes. The draws' registers are still written as the
frame wrote them (the GS's state after the world is the frame's).

The checks on the branch as it stands: `tools/boot_test.py` PASS; `tools/frame_test.py --renderer native` PASS, and
`--renderer fast` (added to the tool with `modern`) PASS: `fast`'s pictures the same every run, and the last one
dumped alone the same.

## The first interface: P2, revised (for agreement)

**What the survey found** (October 2026): the snapshot cannot be made on the game's side as first planned. A draw's
material is not in its `RdDraw`: its texture is the GS's TEX0 (in the batch's A+D packet) read against VRAM *as it
is at that point of the chain*, and its TEST and ALPHA are the chain's registers. `Map_DrawRecords` uploads map 0's
textures, draws map 0's records and objects, then uploads map 1's (packets 0, 1, 5 of each slot) and draws map 1's:
a world drawn whole at the block's end would read map 1's pages for map 0's records. The page-ownership table is
the only place that knows which texture a draw sees, and it lives on the native GS's thread, in chain order.

So the snapshot is made **on the native GS's thread, as the draws are met**: the adapters keep queuing `RD_ND_DRAW`
as now; the native GS, meeting a draw the PC renderer takes, resolves its batches' materials then (the texture from
the table, expanded by P1's cache, kept by a strong handle so a later eviction cannot drop it; TEST, ALPHA, FBA, FOG,
the scene target) and keeps the draw instead of drawing it. What it keeps is drawn, by material, into the scene's
colour and D32F depth (the standalone scene's own targets, `gsn_scene_targets.inc`) when anything else is about to
touch the scene: the first draw it does not take, a PS2-path batch, a scene op, a turn back to the GS, a vblank. This
is how `sa_group_draw` already keeps a skinned model's parts. In P2 that keeps the chain's order exactly (map 0's
objects draw after map 0's records), with no change on the game's side; from P3, when the PC renderer takes the
whole world, the one flush a frame is at the world block's end, and the `RD_ND_WORLD` mark is added then (the game's
`FrameInterp_EndWorld` hook is where it goes). The game-side `rd_world_begin` / `rd_world_end` are dropped.

On the native GS's side (`port/gs/native/gsn_pc_world.inc`, included by `gsn.cpp` after `gsn_standalone.inc`, in its
namespace, like the scene's; the native GS's thread only):

```cpp
// ---- The PC renderer's world (--renderer fast, docs/port/PC_RENDERER.md) -----------------------------------------
// The world's draws the PC renderer takes are kept as they are met in the chain, their materials resolved then from
// the GS's state and the page-ownership table, and drawn by material into the scene's colour and depth at the next
// flush. P2 takes the map's records (RD_KIND_MAP_RECORD, program 0x000) whose every batch is opaque or alpha-tested
// (no ABE, no FBA trick, no shader blending, TME from the table, into the scene's frame); every other draw is
// drawn by the scene as now, after the flush.

// A batch's material: what the draws are sorted and bound by
struct PcMaterial {
    ImageHandle texture;   // P1's RGBA8 with its levels (rgba_view's image, held: eviction cannot drop it), or none
    const Sampler *sampler;
    uint32_t test;         // TEST's alpha test and Z test (ATE, ATST, AREF, AFAIL, ZTST)
    uint32_t tfx_fog;      // TEX0's TFX and TCC, PRIM's FGE
    bool zwrite;           // ZBUF's mask
};

// Meeting RD_ND_DRAW (sa_draw_begin's place): true if the PC renderer took it (kept, nothing drawn now); false and
// the scene or the GS draws it, after pc_world_flush
bool pc_world_take(uint64_t value);
// What was taken drawn into the scene's colour and depth, opaque by material, then alpha-tested by material, the
// draws' order kept within a record (coplanar layers by order and a depth bias, not the scene's Z-first ties)
void pc_world_flush();
// a map change, a save state loaded, the renderer switched: what is kept dropped, the meshes on the GPU forgotten
void pc_world_forget();
void pc_world_print_stats(); // --stats: draws taken, batches, materials, flushes a frame, the GPU's meshes
```

- **Meshes on the GPU** stay in native draw's arena (`nd_place`, cached by `RdMesh`): already persistent. A batch is
  one `vkCmdDraw` with its first instance naming its draw's `NdConst` record, so a material's batches share their
  bindings and pipeline; `nd_000.vert` (built as `sa_000.vert`) is the vertex stage, `invariant gl_Position` so
  that a layer and its surface give the same depths.
- **Checks for P2**: `modern`'s pictures byte for byte as before; `fast` at the five spots against `modern`
  (mean and worst difference, per spot, each explained); the pond's GPU time at the owner's settings against P1's
  3.45 ms; ctest, vu1_test, ASan + UBSan.
- **Later phases**: P5's pictures between interpolate the kept draws (their `RdDraw` identity and matrices are in
  the snapshot as before); only where the snapshot is made has changed.

## P7: the world off the GS's batches (built 2026-10-29)

**What stopped the last attempt, and how it is dealt with.** A PC renderer apart from the GS was stopped by "P2,
revised": a draw's material (TEX0 read against VRAM as the chain has it at that point, TEST and ALPHA the chain's
registers then) is only known on the GS's thread, in the chain's order. P7 keeps that: the material is still resolved
where the GS's thread meets the draw. What it takes away is everything else the GS did with the batch: no corners
planned on the CPU, no GS batch, no `sa_draw_batch`, no runs, no ties' Z-first draws, no texture rows from the
vertices. The batch is drawn at once, in the chain's order, from its mesh on the GPU.

`port/gs/native/gsn_pc_draw.inc` (included by `gsn_standalone.inc` after `gsn_pc_world.inc`; the GS's thread):

- **Program 0x000** (the map's records and sky, objects, items, gold, the picked item): one draw a batch, its triangles
  a list on the GPU made once per mesh (`pcd.tris`: the arena vertex of each corner and the kick bits), the draw's
  `NdConst` record its instance; `pc_000.vert` (`nd_000.vert` built with `PC_DRAW`) does VU1's maths, the ADC that
  leaves a triangle out and a layer's Z slope. Consecutive batches with the same material and record grow the last
  draw (`Cmd::grow_last_draw`): at map 000 2.24 million batches in 2,500 vblanks became 602,000 draw calls.
- **Program 0x280** (the creatures, the arm and what it holds): the CPU's plan (`nd280_plan_cached`) still says whether
  a run draws and which triangles VU1 kicks; those go into a list each time (`pcd.kicked`), each pass one draw with
  `pc_280.vert`. No corners, no GS batch, no skinned groups.
- **Program 0x700** (the effects): the same, one strip a draw with `pc_700.vert`, the layer's slope from the plan.
- **Materials** are cached by the registers they are resolved from, the vertices' alpha range and the CLUT's contents
  (`PcdKey`); dropped when the page-ownership table changes. A material that reads a render target or VRAM (the
  reflection, the scrolling textures, a region clamp) uses the GS's sampling (`gs_frag.h`) and is resolved again
  each time. A draw into a named target (the reflection's 256 x 256) is drawn there.
- **Declined** to the GS's batch as before: shader blending (As above 1, PABE, no COLCLAMP, FBA; about 3% of the
  batches at map 000, 8-12% at 009 and 063), two alpha-test passes, DATE. `KF4_PC_DRAW=0` turns P7 off.
- The pipelines (both samplers, `ps_scene_samples()` x the scratch formats) are in the pipeline set: `KF4_GSN_WARM=set`
  made none outside it.

**Measured** (the owner's settings: 4x, native widescreen, MSAA 2, aniso 16, cut-out SSAA, `--native-colour high`;
headless, `pace_test.py`, P7 against `KF4_PC_DRAW=0`, runs alternated):

| | map 000 | map 009 |
|---|---|---|
| the GS's thread a game frame (cost, 240) | 13.1 ms, was 16.2 (-19%) | 13.9 ms, was 17.0 (-18%) |
| a recording | 4.83 ms, was 4.34 | 4.2-4.3 ms, was 4.63 |
| fixed 240: pictures a second (lowest second) | 239.75 (239), was 236-238 (230-234) | 239.75, as before |
| the GPU a picture at 240 | 3.40 ms, was 3.61 | 3.40, as before |
| unlocked: pictures a second | 256-258, was 198-203 (+28%) | 262-264, was 225-228 (+16%) |

**Pictures** against the GS's batches (2,500 vblanks, the frame and three pictures between): map 000 differs by one
level over whole surfaces (the GPU's interpolation of colour and UV against the GS's corners: 270,345 pixels, worst
18, 2 above 16); 009:0 22 pixels (worst 4); 009:2's creatures 2,374 by one; 063 6,420, of which 127 above 16: a seam
crack the GS's batches drew and P7 does not.

**What still goes through the GS's batches in `fast`**: the declined batches above; the shadows (0x5A0: DATE and the
stencil, about 570 batches at map 000); the GS's own 3D (about 7,000 batches); the water's ripple and the weapon's trail
strips; the 2D and the screen passes (by design, part 1's top). A picture between still walks the frame's packets
again (the recording), and at map 000 P7's recording is about 0.5 ms dearer, its many small draws recorded as calls
rather than replayed GS batches: "Where it stands", next item 1.

### P7, kept: the frame's calls made again in its pictures between (built 2026-10-29)

A picture between walked the frame's packets again and, for each world draw, resolved every batch's material again and
planned its triangles again, for the picture's record. Now the frame keeps what P7 did, and a picture makes it again
(`gsn_pc_draw.inc`, "Kept calls"; `PcdKept` in `gsn_nd_prep.inc`, beside the record and corners nd_prep keeps):

- **The frame keeps each call.** Every P7 call of a draw goes into `NdPrepFrame::pcd_calls`, in the draw's order: a batch
  of program 0x000, a pass of 0x280, a strip of 0x700, and those that drew nothing (`PCD_CALL_NOTHING`) or were not
  kicked or all clipped (`PCD_CALL_UNKICKED`, no PRIM). A drawn call holds its material (`shared_ptr`: the material cache
  may let it go), its triangles' list (a buffer never written again), its first triangle and count, its instance bits
  and the registers its material was resolved from. A draw is kept whole when every one of its batches went to P7 and
  was kept; a declined batch, a material that reads a target or VRAM (`!kept`), an unsupported mesh, or an arena that
  changed while the draw was made, makes it not whole.
- **A picture takes the draw** when the frame's draw at that place is whole, the table has changed no more since the
  frame began than the frame's had (`sat.gen`, counted from each frame's own start: the counter goes on from frame to
  frame) and the arena is the frame's (`pcd_between_takes`).
- **Each call is made again** (`pcd_between_call`). Its registers are checked against the picture's, less what the
  picture has of its own: FRAME's base and width and ZBUF's base (a picture is shown in its own buffer), the scissor
  and the scene's places. Then the place part of `pcd_prepare` runs as it does for any batch (`pcd_where`: what came
  before in the chain is drawn, a reflection kept as the frame drew it, ZTST NEVER, `sa_acquire` from the picture's
  FRAME and ZBUF, the viewport and scissor), and `pcd_finish` draws with the kept material and triangles and the
  picture's record. Nothing is resolved or planned: `nd280_plan_cached`, `nd700_plan` and the corners are not made for a
  taken draw, and 0x700 learns from its kept call whether a strip is kicked (PRIM is written only then).
- **`pcd_prepare` is three parts**: `pcd_key_regs` (the registers, and the declines that draw nothing), `pcd_where` (what
  came before, the target, the place) and `pcd_material` (the material, held when kept). A kept call uses the first for
  its check and the second for where it goes; the third is the frame's, kept.
- **A fallback** (a call whose registers or place differ, or that declines in the picture) turns the draw back to the
  normal path from that call on (`pcd_between_stop`). The calls before it were checked and stand.
- `--stats`: `pc draws, pictures between (KF4_PC_KEPT)`: the draws made again from the kept calls, and those worked out
  again, by why (no frame kept, not whole, the table or arena changed, a call on: its registers differed or it declined).
- `KF4_PC_KEPT=0`: nothing kept and nothing taken, `fast` as before.

**Checked**: every picture (the frame's and `_250`, `_500`, `_750`, every 7th vblank from 2300 to 2650, 204 a spot) byte
for byte the build's before at 000, 009, 009:2 and 063, and `KF4_PC_KEPT=0`'s; frame_test `--renderer native` and
`fast` PASS; ASan + UBSan (clang) at 000 and 009:2, nothing in port/gs.

**Measured** (the owner's settings, `pace_test.py --hz 240 --mode cost`, alternating runs, two each):

| | kept on | `KF4_PC_KEPT=0` |
|---|---|---|
| 000: a recording | 4.85, 4.79 ms | 5.05, 4.93 ms |
| 000: the GS's thread a game frame | 12.79, 12.81 ms | 12.92, 12.97 ms |
| 009: a recording | 5.13, 5.22 ms | 5.12, 5.10 ms |

About 3% of a recording at 000, nothing at 009, because the table check turns most draws away: made again / turned away
by it, 60,046 / 112,491 at 000, 42,794 / 187,606 at 009, 45,669 / 58,410 at 009:2, 86,668 / 0 at 063.

**Open: the table check is a count, and a count cannot say "unchanged".** A picture between changes the table less than
its frame did: it asks only for the uploads it needs again (frame_interp.c's uploads left out), leaves the kept
reflection's draws and the frame's copies out, and draws into the buffer shown. Counting only the named targets' rows,
or only the owners' changes (the uploads and the table made again), turned away the same draws (tried, not kept). With
the check removed (a test run at 009:2) twice the draws were taken and the pictures were the same, but that is not a
proof. The owner's rule (2026-10-29): no shortcut and no workaround; the check is replaced by one of what the call reads
(the table's answer for its texture's pages, the same image as the frame's), not removed or loosened. That check is
part of the next step ("Where it stands", "Next"), where the kept calls become the world's snapshot.

### The world's snapshot: a picture's world draws without the walk (built 2026-10-29)

"Where it stands", next item's steps 1-3, built on the kept calls ("P7, kept", above). `gsn_pc_draw.inc`, "Snapshot";
`PcdSnapDraw` in `gsn_nd_prep.inc`.

**What the frame keeps.** For each draw kept whole (every batch the PC renderer's, every call kept), besides its calls:
the draw and camera as fetched; the 33 registers it began with (`pcd_entry_regs`: everything a call or the record reads
but the vertex, transfer and port registers, FRAME and ZBUF less their bases and FRAME's width, and the scissor: a
picture's own); each TEX0 write among its calls (`pcd_batch_write`: the CLUT load it starts, with TEXCLUT then, and
before which call); and the registers its writes left (those written, and those a write changed besides: TEX0's
mipmap addresses). A draw that writes a vertex, transfer or port register, or FRAME, ZBUF or a scissor, or one of whose
calls drew nothing for its place (the kept reflection, an empty scissor), keeps none.

**What a picture does with it** (`pcd_snapshot_find` at `RD_ND_DRAW`, `pcd_snapshot_draw` in place of the batches):
1. **Its place in the chain.** When the 33 registers are the frame's at the draw's start (exact), the draw is the frame's
   from there: its writes are the meshes' and the draw's, the same data, so every call's registers are the frame's
   (the per-call check of the registers is not needed). Its record is made from the kept draw and camera (`nd_fetch`:
   no `rd_fetch`, no meshes); the GS's vertices pending are drawn first and the scene's kept parts after the loads
   before the first call, as the walk's first call does; then the calls, with their materials, by the picture's
   record. One `pcd_where` for the draw: every drawn call's frame, Z buffer and scissor (its context's, the picture's
   bases) must be the first's.
2. **The state it leaves.** The CLUT loads are made again in order (they read VRAM: `clut_load`, `sa_clut_load`), each
   drawn call's CLUT checked as the frame's after the loads before it (the mirror's hash); then the registers are set
   as the writes left them, without writing them.
3. **The records.** Each draw's record is made from the kept draw and camera, as the walk made it from the fetched ones
   (the same code: the moves by `nd_model_flags`), with no fetch and no batch walked.

**Exact checks** (none is a count): the registers at the draw's start; each drawn call's CLUT; the place (`pcd_where`'s,
and each call's `pcd_place_same`); the table's answer for each call's texture (`pcd_table_same`: `sa_texture` asked as
the material was resolved, the same decode by its serial, not a target; found once a material while the table, the frame
copy and the scene's image are as then). A CLUT load from a page some target has walks the draw (its write-back would be
an act among the draws). Anything that fails walks the draw as before, its loads undone first (the GS's and the table's
CLUT put back). **The kept calls' table count is replaced** by `pcd_table_same` in `pcd_between_call` too: at 000 every
kept draw is now taken (172,537; was 60,046 with 112,491 turned away by the count).

`--stats`: `pc draws, snapshot (KF4_PC_SNAPSHOT)`: the draws made from the snapshot, their calls and loads, the draws
walked by why, and the frame's draws kept whole without a snapshot by why. `KF4_PC_SNAPSHOT=0`: none kept, all walked.

**Checked**: at 000, 009:0, 009:2 and 063 (the start: hex 0x63, 99 to `--teleport`, where the game starts; 204 pictures
each: the frames and `_250`, `_500`, `_750`, every 7th vblank 2300-2650) byte for byte with `KF4_PC_SNAPSHOT=0` and with
the build before; ctest (gsn_draw among them), vu1_test, boot_test, frame_test native and fast; the map tour (64 maps,
its captures made the first time on this machine in 9 min 17 s: 450,157 pixels, worst 106 on map 030, every scanout
the same; the same with the build before); ASan + UBSan (clang) at 000 and 009:2, nothing in port/gs. Made from the
snapshot: 169,380 draws at 000 (208 walked for their registers, 2,949 for a CLUT on a target's page), 226,187 at 009
(474, 3,739), 100,565 at 009:2 (569, 1,608, 1,337 for the place or the table).

**Measured** (the owner's settings, `pace_test.py --hz 240 --mode cost`, alternating, two runs each):

| | snapshot | `KF4_PC_SNAPSHOT=0` |
|---|---|---|
| 000: a recording | 4.37, 4.47 ms | 4.62, 4.65 ms |
| 000: the GS's thread a game frame | 12.42, 12.32 ms | 12.65, 12.62 ms |
| 009: a recording | 4.49, 4.55 ms | 4.94, 4.81 ms |
| 009: the GS's thread a game frame | 14.58, 14.53 ms | 15.34, 15.03 ms |

## Open question

- **P1 under `modern` too** (`--native-sampler hardware`): the plan builds it so, so every map is faster before
  P2-P6; its pictures differ from the PS2's in filtering only. Not decided: `fast` uses it either way.

# Part 2: smooth play (steps 0-5)

The owner's report: on a top GPU (RX 9070 XT, 170 Hz screen) the game still drops pictures, stalls, and logs
`a pipeline not on the list was made in play`; it should be much better than it is, and have an unlocked frame
rate. This plan takes each cause measured below to its fix, in order of what the owner feels, and adds
`--frame-rate unlocked`.

## Smooth play: what was measured (main 74149928 + the refresh clock, 2026-10-28)

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
   **(Corrected 2026-10-29: not at the owner's settings.** The native GS's thread stops the pictures first, at about
   175 a second on map 000: "Where it stands" at the top, and step 3's measures.)
7. **The game's thread hashes about 5 MB of meshes a game frame** (native draw finds its meshes by content: 5.9 GB
   hashed in the run). It keeps up on this CPU (no game vblank over 25 ms in play) but it is the first thing a
   slower CPU or a busier map loses.

## Smooth play: the targets

At the owner's settings, on this machine and on Windows with an NVIDIA card (the work is platform-neutral):

- **No stall from a pipeline, ever**: no gap over two refreshes from a compile in play, including a first run of
  a new build on a cold driver cache.
- **Steady play**: at least 99.5% of refreshes one step of the world, at 170 and 240 Hz, every spot of the map
  tour; no second under the refresh rate.
- **GPU at most 1.5 ms a picture** at 4x MSAA 2x (from 3.46), so that unlocked reaches 500 a second and 240 Hz
  keeps up with room.
- **`--frame-rate unlocked`**, as smooth as `monitor` where the screen can show it (VRR, or tearing allowed).

## Smooth play, step 0: land what is in flight, and measure what the owner sees

- Commit the refresh clock (`refresh_clock.c`, `screen_vblank.c`, `refresh_clock_test`) with its checks.
- **A real window's record.** `KF4_PRESENT_LOG` in a real window, with each picture's present and, where the
  system knows it, the refresh it reached; `pace_test.py --window` reads it. The headless runs above cannot see the
  compositor, the swapchain or fullscreen; this is how the owner's own play gets numbers.
- **`pace_test.py --cold`**: a fresh data folder and the driver's cache off (`MESA_SHADER_CACHE_DISABLE=true`;
  `__GL_SHADER_DISK_CACHE=0` on NVIDIA); prints the compiles made in play, their total and the longest. Step 1's
  check.
- Owner, now: set the frame rate to **monitor** (the Debug page, or `"frame_rate": "monitor"`): it is item 3
  above, and it costs nothing.

## Smooth play, step 1: pipelines never stall play

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

## Smooth play, step 2: the pictures between off the GS's thread

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

**What the pictures wait for** (2026-10-29; map 000, the owner's settings, 169.83 Hz, real time; a diagnostic build
timing each picture from when the controller sends it to when the GS's thread takes it):

| | pictures | wait: median, p90, p99 |
|---|---|---|
| replays in the vblank of the recording | 3,064 | 10.75, 12.5, 14.2 ms |
| replays in the vblank after | 4,715 | 0.6, 4.8, 7.3 ms (none behind the next frame's packets) |
| the recordings themselves | 1,666 | 4.7, 5.9, 7.3 ms, then 5.8 ms to record |
| the exact restore (571 of 1,666 frames) | | 0.76, 4.3, 4.6 ms of the GS's thread |

So a picture between waits for its recording, not for the GS's thread's next packets: the pump (2b) would make the
same pictures at the same times. In the present log this is the steady part of the uneven refreshes: a gap of 4
refreshes (23.6 ms) at the start of a recording's vblank, 1-3 a second.

Sampled in real time (`eu-stack`, both threads): the GS's thread 12% waiting at the vblank for the back end
(`backend_send`), the back end 28% waiting for the GPU (`next_frame_context`: Granite's frame contexts are taken a
picture each, pictures between too, so 4 is 4 pictures in flight), 18% making calls, 51% idle. 8 frame contexts
instead of 4: no difference (10 runs of 25-40 s, within the runs' spread). What is left of a recording is flat:
native draw's batches drawn through `sa_draw_batch`, no line over 2% of the thread.

Also: runs where the game itself falls behind for a second or so (55-58 game vblanks a second, gaps of 77-117 ms),
in about a third of the runs, before this change as after it, with no hitch on the GS's or the game's thread; the
desktop was busy (kwin, plasmashell, a browser playing video). Not found yet. And pace_test.py counted the last
vblank's pictures, which the run writes out as PNGs (0.4 s): that was every run's lowest second (about 114); fixed.

| map 000, 25 s, 169.83 Hz | uneven refreshes a second | pictures a second (lowest second) |
|---|---|---|
| before (d4919d86), three runs | 4.16, 8.76, 1.80 | 168.3 (142), 162.6 (110), 168.7 (150) |
| after (a frame recorded once), three runs | 1.64, 1.12, 1.00 | 169.0 (159), 169.2 (165), 169.3 (164) |

(Alternating runs; the spread between runs is as large as the difference.)

**So, next:** the pump is not built (it buys nothing while the recording is what pictures wait for). What would move
the pictures are less of the GS's thread before and in the recording, and the GPU's time a picture (step 4), which
is what the back end, and through it the GS's thread, waits for at the vblank.

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

## Smooth play, step 3: `--frame-rate unlocked`

As planned, one more policy for the pump (step 2), not a new path:

- **unlocked**: a picture whenever the GPU has room (two in flight), each showing the world at the time it is
  expected on screen (now and the measured latency). The swapchain immediate where tearing is allowed, mailbox
  otherwise (no tearing; only the newest picture before each refresh is shown, for the least latency).
- **a cap** (`--frame-rate-cap N`, and the settings page): unlocked, no faster than N. For VRR screens (FreeSync,
  G-Sync) the cap a few below the screen's rate keeps it in the VRR range.
- The settings page: 30, 60, monitor, unlocked, and the fixed rates under "other"; a fixed rate that is not the
  screen's says it will judder (item 3).

Without VRR and without tearing, a screen shows one picture a refresh, and `monitor` is already the smoothest it
can be; unlocked then gives less latency, not more smoothness. With VRR or tearing it is smoother.

Left as they are, as at every rate above 30: flipbook animations and the HUD's orb change 30 times a second.

### Built (2026-10-29): unlocked as a rate found as it plays

The pump is not built (step 2: it buys nothing while pictures wait for their recording), so unlocked is built on
today's schedule: the pictures between a vblank are still worked out ahead (`port_interp_schedule`, up to 16 a
vblank) and shown at their times, at a rate that follows the renderer.

- `--frame-rate unlocked` (`settings.json` `"frame_rate": "unlocked"`, the Display page's "Unlocked"),
  `--frame-rate-cap N` (`frame_rate_cap`, 0 none; a slider under it), `--tearing` (`tearing`, off).
- **The rate** (`port/include/port/unlocked_rate.h`, `unlocked_rate.c`; `unlocked_rate_test`, ctest
  `unlocked_rate`): between 60 and the cap (or 16 a vblank, 959). At each game vblank gs.c says whether the native
  GS was behind, as the cut judges it, or left pictures between of the vblank before out; and frame_interp.c gives
  the pictures presented a second (the window's half-second measure). Behind two vblanks in a row: down 15% (28%
  when it fell just before). Presented under 90% of the rate asked, 45 vblanks after a change: down to 95% of what
  was presented. Kept up for a second: up 5%, quickly to 95% of the rate it last fell from, past that only after a
  probe's rest that doubles each time a probe falls (to 8 s). The cut of the pictures between judges only at the
  floor. `--stats`: "frame rate unlocked: N a second at the end ..., M times down, K up".
- The second signal is needed: past its limit the renderer leaves pictures out (gs.c's "left out as the renderer
  was behind the game") before its thread is a vblank late, and a fixed 280 at the owner's settings showed 36 of
  the game's vblanks a second on screen without the cut ever judging.
- **The swapchain** (present_vk.cpp): unlocked is mailbox whatever VSync says (the owner's choice), FIFO where
  there is no mailbox; with `--tearing` immediate, then mailbox. SDL_Renderer's presenter keeps VSync as set.

**Measured** (map 000, the owner's settings, `pace_test.py --mode pace`, 169.83 Hz simulated screen, real time;
`--renderer fast --native-msaa 2 --native-colour high`):

| | pictures a second (lowest second) | game vblanks shown a second | uneven refreshes a second |
|---|---|---|---|
| monitor | 159.0 (153) | 56.5 | 15.5 |
| fixed 240 | 188.8 (183); 1,353 pictures left out | 60.7 | 40.8 |
| fixed 280 | 147.4 (141) | **36.5** | 77.8 |
| unlocked, first (behind alone) | 155.8, 154.0 | **48.7, 49.0** | 73 |
| unlocked, built (with the pictures presented) | 155.1 (147), 155.3 (141) | 58.3, 59.0 | 20.2, 18.4 |

At the owner's settings the rate settles about 175 (7 falls, 33 rises in 25 s). What holds it there is not the GPU
(3.1 ms a picture: room for 320) but the GS's thread: at fixed 240, the pictures left out are left out for it (the
recording's vblank, step 2's finding). So at these settings on this machine unlocked gives what monitor gives, and
more only where the renderer has room: at `--native-scale 2 --native-msaa 1` (1.73 ms a picture) it reached 285 a
second and was still climbing. A real window with mailbox is not measured yet (step 0's window record).

## Smooth play, step 4: the GPU's time a picture to 1.5 ms

**Premise corrected (2026-10-29).** This step was planned on item 6's "the ceiling of any unlocked frame rate". At
the owner's settings it is not the ceiling: the native GS's thread is (step 3's measures). The GPU's time matters
again once the thread has room; until then a GPU at 1.5 ms gives the same pictures.

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

### Item 1, built (2026-10-29): the copies in the render pass, in `fast`

Why the passes ended: not the blend equations. Shader blending already copies the destination inside the render
pass for 8-bit single-sampled targets (`COPY_IN_PASS`, the scene's `in_pass`); a multisampled target's copy was a
resolve between passes, and `--native-colour high`'s 16-bit targets (the owner's) were turned away from the
in-pass copy in both paths (`!wide(...)`): every shader-blended run ended the pass and resolved. The in-pass
copies' shaders already handle 16-bit samples (`pass_resolve.frag`'s scale 65535; the scene's run copies took a
fixed 255, now the target's). So `fast` takes `KF4_GSN_MSAA_BLEND=fast` by default (`msaa_blend_in_pass`, gsn.cpp)
and for 16-bit targets too; `KF4_GSN_MSAA_BLEND=exact` gives the resolves back, in `fast` too. `native` and
`modern` are unchanged (their pictures are references).

| map 000, the owner's settings, `KF4_GSN_GPU_TIME=1` | before (`exact`) | after |
|---|---|---|
| GPU a picture (two runs) | 3.418, 3.425 ms | **3.329, 3.335 ms** |
| render passes, resolves a picture | 12.0, 10.2 | 3.1, 0.5 |
| loaded, stored, resolved by passes a picture | 212, 231, 180 MB | 144, 162, 65 MB |

The pictures: map 009:2 (the pond's creatures, the most shader blending), 64 pictures at 120 with every picture
between drawn, 1280 x 600 presented: 82 pixels of 41 million differ, by 1 at most. Pipelines: none made in play
(the set has the scratch passes at every format already).

A pass a run cost less than item 6 implied: 2.6%. Where the rest goes (`KF4_GSN_GPU_TIME=2`, which serializes the
GPU at each mark, so as shares of its 3.33 ms): scene draws 0.55, passes' untagged time 0.50 (10 draws a picture:
the full-screen work at the start of a pass), Z-first alpha-tested 0.35, skinned groups 0.35, GS batches 0.31, the
scene's ops 0.28, MSAA loads 0.27, the in-pass copies of pictures between 0.21, resolves 0.18, copies 0.14. More
than half is full-screen work at 3840 x 1792, two samples, 16 bits a channel, not the 21,000 triangles.

### The other items, measured (2026-10-29)

- **The scene's ops** (the game's depth clear and alpha set, drawn as sprites over the whole scene): left out
  (a diagnostic build, not kept), the GPU's time a picture did not change for the depth clear and fell 0.04 ms for
  the alpha set. Not worth a clear in the pass.
- **Items 2-4 are not built.** Each is a change to how the scene's passes are made (item 2), to the GS's alpha test
  where the colour pass depends on it (item 3), or to the back end's stream (item 4), several days each with the
  checks above; and the measures above say they would not move what the owner sees now: at the owner's settings
  the GS's thread, not the GPU, holds the pictures at about 175 a second (step 3), so a GPU at 1.5 ms would give
  the same pictures until the recording's cost (step 2) comes down.

## Smooth play, step 5: stalls outside the renderer

1. **Map changes.** Measure door and area changes (`KF4_HITCH_MS=25`, `[hitch] game vblank`: disc reads, meshes
   parsed and hashed) at a dozen MAP:PLACE spots; then read the disc image ahead into memory, and parse and hash
   new meshes on a worker thread while the game's own load runs.
2. **The meshes hashed each game frame** (item 7): know an unchanged mesh by where it is and VRAM's write
   tracking, hashing only what changed.
3. **The first picture of each game frame** is drawn from the packets (4.8 ms of the GS's thread each, 2,033 in
   the 000 run; 367 drawn again as the GS was not as recorded): after step 2, measure whether it still matters.

## Smooth play: the order it was planned in, and the owner's choices

Step 0 and step 1's 1-3 first: they are small and take the stalls the owner sees now. Then step 2 (the larger
change: 2a, the recording from the frame's own drawing, then 2b, the pump), step 3 on it, step 4, step 5. Each step lands with its checks, `KF4_DATA_DIR` a temporary folder in
every run, and its numbers here.

For the owner:
- Step 1.4: a pipeline not made yet in `fast`, the batch left out for a few pictures (proposed) or the stall.
- Step 3: unlocked's default without VRR is mailbox (no tearing; **the owner's choice**, 2026-10-28); immediate
  stays an option. Whether fixed rates leave the settings page's main list is still open.
- The targets above.

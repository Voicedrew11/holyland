# The PC renderer (`--renderer fast`)

A real PC renderer for King's Field IV's world: the map, the map objects, the items, the characters and the arm
drawn by our own Vulkan renderer from the game's data and state, the way a PC game draws its world, instead of
the PS2's GS emulated draw by draw. The GS path keeps what is 2D or over the whole screen (the HUD, the menus, the
text, the fades and the screen passes) and, until they are ported, the effects. `--renderer modern` stays the
reference (the PS2's pictures, checked byte for byte); `fast` is checked against it by how much it differs, not
whether, and is free to differ where that is the point (filtering, depth, anti-aliasing, frame rate).

Status: the plan, agreed with the owner (October 2026); P1 built, P2 built (the map's records, objects, items and gold,
opaque, alpha-tested and blended), P3's first two steps built (the skinned parts' first pass drawn once, alphas a
little above 1.0 by Vulkan's blending): at 170 Hz `fast` keeps the owner's settings at 168-170 pictures a second at
maps 000 and 009, where P1 kept 84 and 118 (below, "In real time"). The steps since P2's first were decided overnight
for the owner's review. P4-P6: "Where P4-P6 stand" below; P5's first step built (the owner suspended the rule of agreeing
the interface first while the plan is finished): at 240 Hz map 000 keeps 231 pictures a second where it kept 197. The
owner's choices:
- **The name** is `fast` (`--renderer fast`, the Debug page's choice and `settings.json`'s `display.renderer`).
- **The look** is the original's, as exactly as a PC renderer can have it: the PS2's lighting, fog, colours and
  draw order, the vertex programs as they are. Room is left for a remastered look (the shaders' inputs carry what
  per-pixel light and real fog would need), but nothing of it is the default.

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

## Open question

- **P1 under `modern` too** (`--native-sampler hardware`): the plan builds it so, so every map is faster before
  P2-P6; its pictures differ from the PS2's in filtering only. Not decided: `fast` uses it either way.

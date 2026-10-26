# Native draw P8: standalone

P8 of [NATIVE_DRAW.md](NATIVE_DRAW.md) (3.4 and "P8: standalone"): the scene stops being drawn into the
GS's targets with the GS's Z. This page covers what the first round built (branch `nd5/p8`, from
`native/draw` 73887b93), its numbers, how to work on it, and the remaining P8 tasks split for up to 8
agents. Read [NATIVE_DRAW_TASKS.md](NATIVE_DRAW_TASKS.md) first: how to build, the switches, the
thresholds and the rules all still apply.

**Status:** T1-T6 are done and merged into `native/draw` ("Integration" below has what the merges settled
and the checks on the merged build). T8 (performance) is done ("T8" below): modern keeps up at 240 at the owner's settings and is faster than hosted; `--renderer modern` on Linux's lavapipe is still wrong (pre-existing, open). T7 (review, the owner's sets) remains.

## What is built

`--renderer modern` (the native GS, plus `gsn_set_standalone`). Without `--native-draw` it means
`--native-draw on` (since T1; the first round's: `map,sky,items`). Nothing changes without it: `--native-draw off` is byte for byte the PS2
path, and hosted mode (`--renderer native --native-draw KINDS`) is unchanged.

| Path | What |
|---|---|
| `port/gs/native/gsn_standalone.inc` | the core: the page-ownership table, textures from their source, the scene's targets and its turns with the GS's, a batch drawn into the scene. Included by `gsn.cpp` at the end of its namespace. |
| `port/gs/native/shaders/nd_000.vert` (`ND_STANDALONE`, built again as `sa_000.vert`) | program 0x000 with the scene's own depth: the GS's Z before any rounding (`p.zf`, the moved `zf` in a picture between), plus `pc.zbias` and the layer slope (`pc.zslope`, the corner's alpha) |
| `shaders/sa_depth.frag` (and `sa_depth_ms.frag`), `sa_copy_ms.frag`, `sa_sample.frag` | the scene's depth and colour given back to the GS's Z buffer and frame (sample for sample), and the cut-outs shaded at every sample |
| hooks in `gsn.cpp` (each one line, marked "standalone") | `sa_flush()` in `flush_batch`; `sa_drawn` in `mark_dirty`; `sa_uploaded` in `complete_upload` and `upload_done`; `sa_upload_begins` / `sa_copied` / `sa_sync` in `begin_transfer`; `sa_upload_replayed` in `replay_upload`; `sa_sync` in `take_snapshot`, `vsync_now`, `gsn_read_rect*`, `gsn_write_rect`, `gsn_vram`; `sa_forget` in `gsn_state` loading and `gsn_restore`; `sa_clut_load` in `gs_write` (TEX0, after the GS's own CLUT load and flush); `sa_verify` in `flush_batch` (after `get_texture`: `KF4_SA_VERIFY` only); `gsn_set_standalone`, `gsn_set_scene_map`, `gsn_print_standalone_stats` |
| hooks in `gsn_native_draw.inc` | `sa_draw_begin`/`sa_draw_end` around `nd_draw` in `nd_register`; `sa_waiting()` in `nd_kick` |
| `port/platform/main_host.c`, `gs.c`, `platform.h` | `--renderer modern` (`port_options.renderer_modern`), its default kinds, its `--stats` lines; `gs.c` tells the GS the map at each vblank (`gsn_set_scene_map`, for the counts a map) |
| `tools/texture_pack.py` | texture packs: the dumped uploads as pictures and back, a CLUT tinted |
| `tools/native_draw_compare.py --mode modern` | the same game hosted, then modern; the `[stats] standalone` line in `report.txt` |

### How a scene batch goes

1. **Which draws.** `nd_register` meets a draw. `sa_draw_begin` fetches it, and `sa_kind` says whether
   it goes to the scene: program 0x000 and a kind in `sa.kinds` (`RD_KIND_MAP_RECORD`, which includes the
   sky, and `RD_KIND_ITEM`; `KF4_SA_KINDS=0xMASK` of `1 << RD_KIND_*` to try others). A batch holds the
   scene's corners or none. A change either way flushes the batch first, and so does a PS2-path kick
   while the scene's corners wait (`nd_kick`). `nd_draw` then runs as in hosted mode: the A+D registers go
   through `gs_write` (the GS's state stays the PS2 path's), and `nd_plan` makes the corners (VU1's
   clipping, the arena, the records).
2. **The flush.** `flush_batch` calls `sa_flush()`. A batch of the scene's corners goes to
   `sa_draw_batch`. Anything else first ends the scene's turn (`sa_release`) and is drawn by the GS as
   before.
3. **`sa_draw_batch`** works out the batch's state as `flush_batch` does (FRAME, ZBUF, TEST, ALPHA,
   the texture, the alpha test's passes, cut-outs) and draws it into the scene's targets, with
   `sa_000.vert` and the GS's own fragment shaders (`gs.frag`; `sa_sample.frag` for cut-outs), so the
   colours are the GS's arithmetic.
   - **Depth.** The scene's depth is D32F holding the GS's Z unrounded (Z / 2^24, the same numbers the
     GS's D32F Z buffer holds), larger nearer: reversed Z, since the GS's Z is a + b/w. It is not kept
     within the Z format at the vertex: a vertex past it, under the eye, tilted the lava's plane.
   - **Ties.** The data counts on a later draw over the same surface winning (layers, overlays,
     signs, the hills' edge on the lava). A draw tested GEQUAL draws its Z first, exactly, then its colour
     within a slack, with no Z written: one Z for a draw that blends by an As that is not 0x80
     everywhere, or adds; half a Z for the others; and for a layer (the first kind writing Z), half its
     triangle's Z change across a GS pixel more, at most one Z (the corner's alpha carries it). This is
     `--native-layer-z tolerant`'s rule, without the GS's rounding, so there is no sawtooth where surfaces
     cross and no lace where two meshes' triangles differ. `KF4_SA_SLACK=LAYER[,TIE]` (tests) changes the
     slacks. A layer of native draw's own corners is now by its real depth instead: two world units across its
     surface, exact over its own triangles ("Distance" below).
   - **Blending.** Vulkan's where it can do it. Where it cannot (DATE, a partial mask, PABE, no COLCLAMP,
     FBA with As, As above 1.0, Ad, FIX above 1.0) the shader blends, from a copy of the scene's colour
     (resolved if multisampled) taken before each run of triangles that do not overlap. A texture that is
     a render target is blended by Vulkan when it can be, not in the shader as `flush_batch` does for the
     frame passes' feedback (the lava over its scrolling target: a level off at most).
   - **Declined** (`[stats] standalone ... declined`). The batch goes back to the GS whole: another target
     (FRAME not 32 or 24-bit, or narrower than 512: the reflection's 256 x 256, the scrolling textures'),
     the frame and Z buffer that cannot be had together, a texture that is the frame itself, and
     multisampling on a GPU without sample-rate shading.
4. **The turns.** `sa_acquire` takes the frame target's picture and Z buffer into the scene's targets
   (copies: the multisampled images when `--native-msaa` is above 1), the first time a scene batch comes
   after the GS has drawn. `sa_release` gives them back before anything else of the GS touches them: a
   batch it draws, an upload or a copy, a read-back, a picture, a save state. Release copies only the rows
   the scene drew into: the colour sample for sample, and the depth as the GS keeps it (with its fraction
   under `tolerant`, rounded to whole Z under `ps2`). It marks those rows dirty. So the GS draws
   everything not in the scene yet (entities, effects, the arm, shadows, water, the passes, the HUD, the
   menus) over the scene's picture, in the chain's order: the 2D and the passes after the world are
   composited over it as the PS2 drew them. With map records and items in the scene there are about 2.3
   turns a frame on map 000.
5. **Pictures between.** The scene's draws, copies and passes go through `Cmd` like the GS's own, so the
   recording replays them. `sa_000.vert` moves the corners as `nd_000.vert` does (the same code), and the
   depth is the moved Z unrounded.

### Textures from their source: the page-ownership table

- **What it records.** For each 256-byte block of VRAM: the uploads that own part of it, in the order they
  came, each with the nibbles of the block's 64 words that are still its own (`SaCover`, 512 bits) and the
  table's count when it was written. An upload takes its nibbles from those before it, which keep the
  rest; one left with none is dropped, so there is no limit on the owners (the most seen on the 64-map
  tour: 2). The nibbles are what make the formats that own part of each word exact: PSMCT24 and Z24 the
  low 24 bits, PSMT8H the top byte, PSMT4HL and PSMT4HH a nibble of it; a CLUT's 16 entries, a 4-bit
  texture's half a byte, a rectangle's edge own their pixels and no more. For each 8 KB page: the count
  when a render target was last drawn into it (`mark_dirty`) or a copy wrote it (`sa_copied`), kept for
  its low 24 bits and its top byte apart (a 24-bit frame drawn over a page leaves PSMT8H and the 4-bit
  high textures in its top byte theirs). An owner whose page was drawn into after it in the nibbles a
  texture reads is not the upload's any more, and a texture's nibble that no upload owns is not the
  table's either (`sa_owners`: every nibble the texture reads, of every block).
- **The chain's order.** The uploads are recorded on the GS's thread as the transfers complete
  (`complete_upload`; `upload_done` for one written as it came, read back from VRAM). That is the order
  of the CPU's chain, so the owner a draw sees is the one the PS2 would have sampled. A picture between's
  uploads asked for again (`replay_upload`) give their blocks back to the frame's uploads (`sa.uploads_frame`
  mirrors `gsn.cpp`'s `uploads_frame`).
- **Why not the upload functions.** The plan had `TextureTable_Queue*`, `Map_QueueSlotPacket`,
  `Effect_UploadTextures` and the rest tell the table. The transfers they queue reach the GS in exactly
  that order and carry the same bytes (the game RAM chunk the DMA REF sends), and the GS sees every
  writer, including those no hook would know. This is P0-3's rule for meshes ("the hash catches every
  writer"), applied to textures. No game code changed.
- **A chunk** is an upload's bytes and registers (BITBLTBUF, TRXPOS, TRXREG), kept once: found again by
  `upload_hash` and a byte compare, so an upload sent every frame is one chunk. The oldest unused for two
  frames are let go beyond 256 MB.
- **Decoding.** A texture all of whose blocks (every level's: the part the batch reads, as
  `flush_batch` works it out) belong to uploads is written into a scratch VRAM (4 MB, `gsn_vram.c`'s
  model): its blocks made zero, then its chunks oldest first. It is then read back as `decode_texture`
  reads VRAM, into the same raw image (indices for PSMT8 and PSMT4) that `gs_frag.h` samples, and found
  again by what it is plus its chunks' ids. So a texture is decoded once per source, however VRAM is
  shared. `KF4_SA_NO_TABLE=1` (tests) uses the GS's textures instead. The pictures were the same on the
  herb run: the table is exact.
- **The CLUT** (T4) is the table's too: `sa_clut_load` (TEX0 written, after the GS's own load and flush)
  mirrors the GS's CLUT buffer (`clut_load`): CLD 0-5 with the mirror's own CBP0/CBP1, the 8-bit and 4-bit
  formats, CSM1 (the 16 x 16 or 8 x 2 rectangle, entries 8-15 and 16-23 of each 32 swapped) and CSM2
  (TEXCLUT's line), CSA, the 32 and 16-bit CLUT formats. A load whose nibbles all belong to uploads is
  read from those uploads written into the scratch VRAM (found again by what it is and its uploads); one
  that is not is read from the GS's VRAM, its halfwords marked. A scene batch takes its colours from the
  mirror (`sa_clut_colours`, `clut_colours`' arithmetic), or the GS's buffer whole when one halfword it
  reads is marked. A scene batch waiting keeps the colours it was made with (the mirror changing alone, a
  texture pack's upload in it, flushes it).
- **What still comes from the GS:** a texture that is a render target (the scrolling textures' 128 x 128
  targets, the reflection, the frame copies) and one on a page drawn into or copied over since its upload
  (`MapScroll_UpdateCopy`'s in-place scroll: a local copy from 0x3C40 back over the texture, on 008, 018
  and 097): T3's. A TEX0 of a format the GS does not have (psm 0x10, on 103) is the GS's (its VRAM).
- **Checking it** (`KF4_SA_VERIFY=1`): every batch's texture, the GS's batches (`flush_batch`, after
  `get_texture`) and the scene's, as the table has it against the GS's VRAM, every row of every level, and
  the CLUT the mirror gives against the GS's buffer; `--stats` counts them (2 logs each that differs, 3
  also each not the table's, 4 each TEX0 once). On the 64-map tour in modern: 1,971,324 textures the same
  and 0 different (49 more before the psm-0x10 fix), 1,970,336 CLUTs the same and 0 different; the GS's
  batches' textures not the table's: 598, all the in-place scroll's. The font (PSMT4 at 0x3B40, its two
  sheets by CLUT at 0x3FE0 and 0x3FE1, CLD 4) is among them: the same wherever text was drawn on these
  runs (the title, the opening's messages, PAUSE). No run showed an item's name yet (picking up the herb
  by pad did not take); that check is still open.
- **`--stats` a map.** `[stats] standalone table` (CLUT loads by where they came from, the scene's CLUTs,
  the most owners of a block, what `KF4_SA_VERIFY` found, the texture pack), and `[stats] standalone map
  NNN` for each map the game was in (`gsn_set_scene_map` from `gs.c` at each vblank, the counts that grew
  since the last vblank given to it; -1: no map, the title): the scene's batches, the declined, its
  textures by where they came from, its CLUTs, and the verification.
- **Texture packs** (the mods' groundwork). An upload is known to a pack by its key: a hash of its format,
  its rectangle's size and its bytes, not where it went, so a pack follows a texture wherever the game puts
  it. `KF4_SA_PACK_DUMP=DIR` writes each upload a scene texture or CLUT came from, once: `DIR/<key>.bin`
  (its bytes as sent) and `DIR/<key>.txt` (format, size, where it went, the map). `KF4_SA_PACK=DIR` reads
  `DIR/<key>.bin` in place of an upload's bytes when the game sends it (the same size only): the scene
  decodes the pack's bytes (textures and the CLUT mirror), the GS's VRAM keeps the game's, so only the
  scene changes (the GS's batches, and hosted mode, draw the game's). `tools/texture_pack.py` makes
  pictures of the dumped uploads (`show`; PSMCT32 RGBA as the GS has it, the indices as grey), puts edited
  pictures back (`pack`; exact both ways: the 42 uploads of the herb run come back byte for byte), and
  tints a colour upload or part of it (`tint`). The game uploads its textures as PSMCT32 rectangles read
  as PSMT8 and PSMT4, and a map's CLUTs as one PSMCT32 upload (099's: 64 x 320 at 0x3D40, 80 CLUTs of 16
  x 16): to recolour a texture, tint its CLUT's 16 x 16 in that upload. The proof: the rock wall of 099
  (TEX0 0x8007a80265423140, CBP 0x3D40) tinted blue on the herb run, the floor, the herb and the HUD
  as they were (`texture_pack.py tint DUMP PACK 7347638491fdb72e 0.4,1.3,1.9 --rect 0,0,16,16`).

## T1: every program in the scene (branch `p8/t1`)

`--renderer modern` now means `--native-draw on` (`PORT_ND_ACCEPTED`), and the scene takes every ported kind:
map records and the sky, objects, items, gold, entities, the arm and the held weapon, its trail, effects,
shadows and the picked item (`sa.kinds`; not the reflection's water, T3, nor the screen passes, the GS's 2D).

- **Programs.** `nd_280.vert` and `nd_700.vert` have `ND_STANDALONE` blocks (built again as `sa_280.vert` and
  `sa_700.vert`): Z unrounded, with the tie slack and the slope term, as `sa_000.vert`. `sa_draw_range` draws a
  batch a stretch a kind, as `nd_draw_range`: 0x000's and 0x5A0's with `sa_000.vert`, 0x700's with
  `sa_700.vert`, 0x280's with `sa_280.vert` and their records, the PS2 path's corners (the trail; the scene's
  ops) with `gs.vert` (`nd_strip.vert` in a picture between).
- **A skinned model's second pass** (0x280's sphere-mapped shine) is drawn at its first pass's very corners,
  the same depth to the bit: no slack and no slope. With the layer slack it shone onto a part a Z or two in
  front (the seated man's thigh under his coat at the far save, 4x: a light band).
- **The shadows (A17).** The scene's depth is D32F S8 where the GPU has it. A run of shadow batches (0x5A0's
  records, DATE with DATM 0) draws where the stencil is not the run's number and writes it there: first
  write wins. The blending stays the GS's, in the shader, from one copy of the scene's colour a batch (no
  runs of triangles that do not overlap: the stencil keeps a pixel to one). The stencil is cleared when the
  scene takes the frame (its depth is then copied in by a pass, `sa_depth.frag`, as a depth with a stencil
  cannot be copied from the GS's D32F). `KF4_SA_NO_STENCIL=1` (tests): DATE in the shader as before. The
  fight's pictures are the same either way.
- **The scene's ops** (`sa_scene_op`): the GS's sprites the scene draws itself, so that it keeps the frame
  across them: `GsDraw_ClearDepth` (Z 0, every pixel failing the alpha test into Z only) before the picked
  item and the arm, and, while the scene has the frame, `GsDraw_SetScreenAlpha` (black, failing into the frame
  only, blended as Cd) before the shadows.
- **Fewer turns.** An upload or copy ends the scene's turn only when it touches the frame or Z buffer the
  scene has (`sa_transfer`, the `begin_transfer` hook; before, every texture upload between the world's draws
  did), and a batch of the GS's elsewhere in VRAM (the scrolling textures' sprites) is drawn while the scene
  keeps the frame (`sa_gs_touches_scene`).
- **Hooks** (reported): `gsn.cpp`: `begin_transfer` calls `sa_transfer(dir)` in place of `sa_sync()`, and `Cmd`
  has `set_stencil` (`OP_STENCIL`, recorded for the pictures between). `main_host.c`: the modern default
  kinds. `tools/build_shaders.py`: the two variants.
- **Debug:** `KF4_SA_LOG=2` prints only what ends each turn (1: also every batch and each decline).

**Numbers** (hosted against modern, `--kinds on`, the same exe; the first round's build, e8a89c00, in brackets:
its modern drew map, sky and items; headless, other agents' games running):

| set | 1x | 4x |
|---|---|---|
| herb | 53 (40) | |
| 000 start | 1,600 (1,233) | 22,688 (16,959) |
| 009 start | 195 (6) | 2,047 (32) |
| 016's bats | 299 (0) | 3,062 (102) |
| the fight (`strong3`) | 6,337 (6,297) | 60,701 (60,413) |
| the far save | 603 (48) | 4,327 (215) |
| spells 3, 36, 66 | 3,146 each (2,443) | 22,385 each (16,870) |
| 008's water (`m8w`) | 2 (4) | 73 (94) |
| 009's water and the miners (`m9w`) | 398 (121) | 5,866 (1,679) |

What the new pixels are: the edges of the models now in the scene (silhouettes, where parts meet or cross:
the torch stands' legs, a side face a pixel wide at a grazing angle, the bats' wings, the seated man's hand on
his thigh). No 8 x 8 block anywhere has more than half its pixels off by more than 8 (the far save's thigh,
before the second-pass fix, was the one). The shadows match hosted (each pixel darkened once), the spell
bursts too. At 240 (game frames; pictures between, a picture): walk on 000 3,159 in 4; 791 (base 556); the far
save 618; 155 (27); spell 3 6,303 in 8; 752 (546); 009's water 744; 219 (29); the fight 6,424; 96 (84).

**Turns** (`[stats] standalone ... turns a picture`): 000 0.93, 009 0.99, 016 0.89, herb 0.88, the spells 0.97,
the far save 1.05-1.26, `m9w` 1.57, the fight 2.16 (was 3.3 on herb before the transfer and elsewhere rules).
What still ends a turn mid-world: **map-record batches the PS2 path draws** (textured, fogged strips of 27 to
375 vertices, `KF4_SA_LOG=2`: prim 0x7c, ALPHA 0x44 and 0x48, at the map records' Z: draws the adapter
declines), the reflection's draws (declined: another target) and the HUD.

**GPU time** (the far save, `quick_far`, 4x, MSAA 2, native widescreen, 240, `KF4_PRESENT_SIM=240`,
`KF4_GSN_GPU_TIME=1`, 1,400 vblanks, two runs each, other agents' games running): hosted 3.72-3.89 ms a
picture; modern 2.73-2.76 (the first round's modern 2.69-2.79). Modern still falls behind at 240 (about 1,580
pictures against 5,230: T8).

**Not done:** water (its records and the reflection's texture: T3); the PS2 path's leftover map batches
in the scene (with `gs.vert` their Z would be the GS's whole Z, below the scene's fraction: a layer over a
native surface would lose its tie; T2 or a P1 follow-up). Every modern run that reaches the end exits with
127, or 0xC0000005 / 0xC0000374 at 240, after printing its stats: the first round's build does the same
(open, T8).

## T2: one turn a frame, the scene's own scale, the composite (branch `p8/t2`)

**One turn a frame.** The scene now keeps the frame from the world's first draw to the 2D:
- **The GS's clear** (libgraph's at the swap: untextured sprites of one colour, Z 0, the whole frame) is
  noted (`sa_note_gs_batch`, `sa.clear`). While nothing else of the GS touches that frame and Z buffer, the
  scene takes the frame by clearing its own targets the same: no copy (`sa_acquire`). Not on the maps whose
  modern sky (`RD_DRAW_FAR_Z`) the GS still draws first (002, 013, 021, 027, 098, 099: T5's).
- **The GS's own batches the scene draws** (`sa_gs_batch`, while it has the frame): the PS2 path's 3D (the
  map records native draw does not take, prim 0x7c: the turns T1 left), with `gs_zclamp.frag`, Z kept fine
  and the tie's slack plus half a Z (their Z is VU1's whole one, the scene's has its fraction); the reflection's
  water (`RD_KIND_WATER`, hosted's `nd_water.vert` with the same fragment program); flat fills over the whole
  screen (`GsDraw_FillScreen` from the player code); and **the screen passes** (`nd_pass_draw` marks them,
  `sa_pass`). In a picture between the GS's 3D and fills take their corners from `interp.vertices`, made on the
  GPU from the frame's kicks as `flush_batch` has them; a pass's are the CPU's (`nd_pass.vert`).
- **A pass that reads the frame** (the brightness, a tint, the flicker filter, haze) reads a copy of the
  scene's colour, at the scene's scale. **A copy of the frame elsewhere** (`GsDraw_CopyFrame` to 0x2BC0, which the
  wobble under water and in the MP drain reads) gives the GS the frame and keeps it (`sa_give(true)`), and the
  scene keeps its own picture as copied (`sa.fcopy`), read for a texture there until those pages are written.
- The GS's sprites and target-reading draws blend in the shader where Vulkan's blending would round
  `(Cs - Cd) C + Cd` (as `flush_batch` does), so the passes come out as hosted's.
- What ends the turn is then the 2D (the HUD, messages) or the vblank. Turns a picture (`--stats`), 1x:
  herb 0.86 to 0.21, 009's water (`m9w`) 0.81 to 0.27; 000, 016, the far save, the spells, the fight 0.21-0.44
  (one a frame; a picture is two vblanks).

**The scene's own scale** (`--scene-scale 1-4`, `--scene-msaa 1|2|4|8`; by default the GS's, and then
nothing below applies). `sa_shape`: the scene's targets are the frame target's size times the scene's
scale, with the scene's samples; its draws set the viewport and `M_SCALE` to it. Where it differs from the GS
target's, the copies resample (`shaders/sa_resample.frag`): taking a frame from the GS repeats each GS pixel
(colour and depth, under its middle), giving it back makes each GS pixel the mean of the scene's pixels and
samples over it (depth: the one under its middle, rounded as `sa_depth.frag`).

**The composite** (`gsn_composite.inc`, `shaders/sa_composite.frag`). When the scene gives the frame back for
good (`sa_hold`) it keeps its picture (hi: its samples resolved) and the GS's frame as given (lo). The GS draws
the 2D over its frame at its own scale as always. Of what it draws over that frame, the batches that are one
colour over the whole screen are kept in order, the chain: a flat colour blended (fills, fades, the dim, the
pause's shade; ALPHA with As or FIX), set, or the frame read a texel a pixel and modulated (the brightness,
the tint). At the scanout (`scanout_to_image`, a hook) the picture is made at the scene's resolution: a pixel
whose GS pixel is what the chain makes of lo (within 2 levels: Vulkan's blending rounds) has nothing of the 2D
over it and is the scene's, through the chain in the GS's integer arithmetic; any other is the GS's, its pixel
repeated (the HUD, messages, menus and title at the GS's scale). Why not the plan's transparent layer: the 2D
needs the frame's own alpha (the MP orb's mask: FBA, then DATE), and passes after the world read the whole frame
(the brightness, last, over the HUD too), which a layer's colour and coverage cannot carry. A frame kept is good
for the vblanks that show it: the GS's clear of that buffer, or a fourth vblank, lets it go. Headless dumps
of such a frame are the composite's (`--dump-frames` pictures at the scene's size).

**Numbers.** Same scale (the defaults): hosted against modern, the same exe, every set of T1's at 1x and 4x
and at 240 (herb; 000; 009; 016; the fight; the far save; spells 3, 36, 66; `m8w`; `m9w`; 240: walk on 000, the
far save, spell 3, 009's fire and water, the fight, the owner's settings on 000) is **the same pixel count as T1's
to the pixel**, with the water, the PS2 path's batches and the passes now in the scene.

Modern with the GS at 1x and the scene at 4x MSAA 4, against hosted at 4x MSAA 4 (`jobc.sh`; the HUD differs
by design, its corner left out; pixels a picture off by more than 8 / 32):

| set | outside the HUD corner, > 8 | > 32 | what |
|---|---|---|---|
| herb | 96, 38 | 10, 1 | edges |
| 000 start | 2,804, 2,904 | 271, 275 | edges (T1's 4x) |
| 009, 016, the far save, the spells, the fight | 170-2,900 | 3-278 | edges |
| 008's, 009's water | 650-7,000 | 4-470 | the reflection, its scrolling textures at the GS's 1x (T3) |
| under water (forced: the copy, the wobbles, the tint) | 4,598, 770 | 15, 0 | the lava's scrolling texture at 1x |
| the frame before (bit 40), aiming | 465-1,070 | 0-2 | it reads the GS's other buffer, at 1x |
| the brightness 0x40, 0xFF | 36, 0; 166, 12 | 0; 38, 0 | edges |
| a fill, the fade, haze | 0-96 | 0-10 | edges |
| the dim and the pause | 12,096 | 7,050 | the PAUSE letters, the GS's 2D at 1x |
| the flicker filter (`--flicker-filter`) | 590, 411 | 0 | a level or two over the frame |

At 240 the pictures between are composited too (the far save: 12 of 12; between pictures as their frames).

**GPU time** (`quick_far`, 1,400 vblanks, `KF4_GSN_GPU_TIME=1`, no other games running; ms a picture):

| | 30 | 240 (`KF4_PRESENT_SIM`) |
|---|---|---|
| hosted 4x | 2.16-2.17 | |
| modern, GS 1x, scene 4x | **0.83-0.85** | |
| hosted 4x MSAA 4 | 2.20-2.29 | 2.63 (5,234-5,237 pictures) |
| modern, GS 1x, scene 4x MSAA 4 | 2.82-3.11 | 5.54 (3,581 pictures) |
| modern 4x MSAA 4 (same scale) | 2.12-2.26 | 5.91 (3,287 pictures; T1's build 2.41 for 1,577) |

Without MSAA the composite is well below hosted. With the scene's MSAA it is not: its render passes
(`pass msaa`) cost about 0.7 ms more than the same scene at the GS's MSAA, not found yet (T8). At 240 modern
draws twice T1's pictures but still falls behind (T8).

**Not done:** the modern sky (`RD_DRAW_FAR_Z`) stays the GS's (T5): at another scale it is the GS's resolution,
and the frame is taken by a copy, not the clear. The reflection, the scrolling textures and the menus'
backdrop are the GS's targets at its scale (T3). The frame before (bit 40, aiming) reads the GS's buffer. 2D
drawn see-through over the world shows the world at the GS's scale under it. A fill or brightness whose colour
moves in a picture between (the GPU's) is not in the chain: those pictures are the GS's where they differ.
The overlay has no switch for the scene's scale yet. Every modern run may still crash at teardown (T8): at 240 with the
composite, 3 of 5 timing runs did, before printing their stats.

## T3: render targets by name (branch `p8/t3`, from `p8/t1`)

`port/gs/native/gsn_scene_targets.inc` (included at the end of `gsn_standalone.inc`) is the table's target side.

- **Named targets.** A colour buffer narrower than the frame (under 512 pixels: the reflection, 256 x 256 CT16S at
  0x2BC0; the scrolling textures' 128 x 128 targets; 0x3A40; the refracting entity's copy at 0x3540; ...) is a
  scene target named by FRAME's base, width and format, with images of the scene's own (the GS's colour format and
  scale). Its pages are its own in the table (`sat.page_named`, with `page_drawn`): a texture all of whose blocks are
  on its pages, drawn into after their upload, is read from its image (`sat_texture`, as flush_batch reads a
  target: `SRC_TARGET_*` at the offset of its first page). `[stats] standalone targets` counts them.
- **Who draws it.** The scene, for the 3D it draws there: `sa_draw_batch` no longer declines "another target" for a
  named one; `sat_acquire` binds it as the scene's frame (its images in place of the frame's, its own D32F S8 depth;
  no copies) and `sat_release` lets it go. The reflection's clear (`GsDraw_ClearFrame`'s black sprite) is the
  scene's (`sat_flush`): colour and depth cleared. **The depth is cleared to the far plane**: the PS2 tested the
  reflection against the main Z buffer's last frame read 256 wide, which can only make holes (hosted shows them as
  dark notches along 014's water edge; modern has none). The GS, for what it draws there (the scroll sprites, a
  sprite copying the frame, the PS2 path's map strips the scene does not take): the scene's image is made again
  from the GS's target when the scene next reads it (`sat_refresh`, with the GS's Z when the scene's was given
  back). The GS's target is given the scene's picture only when the GS wants it (`sat_give_back`): a batch of the
  GS's that draws there (with the depth when it uses Z) or reads it, a transfer over it, and `sa_sync` (colour only:
  a vblank, a read-back, a save state). A batch all outside its scissor gives nothing back.
- **The water in the scene.** `RD_KIND_WATER` (`RD_PROGRAM_RIPPLE`) is in `sa.kinds`: `nd_water.vert` has an
  `ND_STANDALONE` block (built again as `sa_water.vert`: Z unrounded, the tie slack), and `sa_draw_range` takes it
  for a batch of water corners. It reads the reflection the scene drew, so the reflection never goes back to the
  GS within the frame.
- **Copies from the table** (`sat_copied`). A local copy whose source is all the table's uploads
  (`MapScroll_UpdateCopy`'s in-place scroll through 0x3C40, on 000, 008, 009) is made in the scratch VRAM
  (`gsn_vram_copy` over the source's uploads) and its destination kept as an upload of its own: the scrolled
  texture is the table's, exact, and found again when the scroll comes round. The GS's VRAM is not read.
- **Mode 1's mirror** (041). A fill over the frame the scene has (`GsDraw_FillScreen`: untextured, failing into the
  frame only, Z not tested; `Map_CoverScreenWithMapTint` between the mirror and the real view) is the scene's op
  (`sat_fill`, `SA_OP_FILL`), blended in the shader from a copy, as flush_batch blends a sprite (the GS's
  arithmetic). So the mirror, the tint, the depth clear and the real view are one turn.
- **Hooks** (in `gsn_standalone.inc`, one line each): `sa_drawn`, `sa_copied`, `sa_texture`, `sa_acquire`,
  `sa_release`, `sa_draw_batch` (the decline test; `sat_blend_exact` in `shader_blend`), `sa_draw_range` (the water
  program), `sa_flush` (`sat_flush`, `sat_fill`, `sat_gs_batch`), `sa_transfer`, `sa_sync`, `sa_switch`,
  `sa_forget`, `sa_print_stats`; `sa_kind` takes `RD_PROGRAM_RIPPLE`. None in `gsn.cpp`.
- **Switches:** `KF4_SA_NO_NAMED=1` (tests): none of this. `KF4_SA_LOG=1`/`2` also say why a named target is given
  back; `3` also each GS draw into one.

**Numbers** (hosted against modern, `--kinds on`, 1x unless said; T1's build in brackets):

| set | `a render target` / `drawn or copied over` | declined "other target" | turns a picture | pixels |
|---|---|---|---|---|
| 000 start | 0 / 0 (622 / 0) | 0 (0) | 0.93 (0.93) | 1,600 (1,600) |
| 008 `m8w` | 0 / 0 (0 / 136) | 0 (16,957) | 0.92 | 2,320 (2) |
| 009 `m9w` | 0 / 0 | 0 (43,813) | 1.03 (1.57) | 97,208 (398) |
| 014 `m14c` | 0 / 0 | 0 (17,463) | 1.08 (1.35) | 33,592 |
| 041 `m41d` | 0 / 0 | 0 | 0.54 (0.81) | 3 |

- The review's other sets are unchanged (herb 53, 009 start 195, 016 299, the fight 6,337, the far save 603, the
  spells 3,146-3,147); `quick_lava` 48 and `quick_sky2` 0 as T1's build (the scroll targets are the GS's picture,
  so `--seamless-scroll` is the same), `quick_teeth` 242 and `quick_underwater` 134,708 as T1's.
- **What differs on the mirror maps is the reflection's colour.** The GS keeps it at 16 bits, rounding every blend
  into it to five bits a channel; the scene keeps 8 and rounds once, when the water reads it (`SRC_TARGET_CT16`).
  The water comes out 1 to 4 levels lighter (009: of 158,720 water pixels, 26,905 more than 2 off, 19 more than 8).
  This is A9's "CT16-like quantisation"; the geometry is the same (the figure's reflection, the pillars' on 014).
  Where hosted differs in shape it is the PS2's holes (014's water edge).
- At 240 (`turn9.pad`, `walk9.pad`, 009, every picture): the reflection moves with the camera in the pictures between
  as in the frames, the same lighter water (about 46,000 pixels a picture, frames and pictures between alike).
- Named targets are single-sampled (the GS draws the reflection multisampled under `--native-msaa`).

## Integration: T1-T6 on `native/draw`

T1, T2, T3, T4, T5 and T6 are merged into `native/draw` (merge commits, in that order, from e8a89c00). What the
merges had to settle, besides putting both sides' hooks next to each other:

- **T2 with T3.** `sa_give` (T2's `sa_release`) lets a bound named target go first (`sat_release`). A texture
  is T2's copy of the frame (`sa.fcopy`, 640 wide at 0x2BC0) before it is a named target (the reflection, 256
  wide at the same base: told apart by TBW). A draw into a named target that reads that target is declined as
  before T2 (`SaTexture::own_named`); T2's copy of the scene's own colour is for the frame. T2's water batch
  (`sa.gs_water`, hosted's `nd_water.vert`) stays for a `KF4_SA_KINDS` without `RD_KIND_WATER`; with it (the
  default) the water is T3's `sa_water.vert`. `sa_transfer` keeps T2's watch on the GS's clear with T3's hook
  first. A copy the table makes (`sat_copied`) lets T2's frame copy there go too. `sat_acquire` keeps the scene's
  scale and puts the GS's while a named target is bound (named targets stay at the GS's scale: T2's open item).
- **T3 with T4.** T3's named targets were a global `sat`, the same name as T4's table: they are `snt` now.
  T3's `sat_own_rows`, `sat_texture` and `sat_copied` use T4's table (`sa_pages_drawn` by nibble, owners with
  their covers, `sa_owners`).
- **T5.** `sa_kind` takes `RD_DRAW_FAR_Z` for program 0x000's records only.
- **T6 with T4.** The table in states and captures is T4's now: record "SAT2" (the layout is in
  `gsn_standalone.inc`, "The table in save states and captures"): each block's owners with their nibble covers,
  the pages' low and high drawn counts, and the CLUT mirror. An owner naming an upload the saver let go gets an
  id no chunk has ("upload let go"). T6's "SAT1" states and captures do not load (the table is left empty: the
  GS's textures until the next uploads); nothing outside this branch wrote them.

**Checks after the merges** (`--no-audio`, a fresh data folder, at most 2 games at a time):
- boot_test, frame_test gs and native PASS; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0 of 90.
- Herb with `--native-draw off` against play-932801f: the same log, PATH1 204e443a9fc49731, VU1 cycles
  460424273, pictures 1800/1820/1840 byte for byte. Hosted `--native-draw on` against native-e8a89c00: the
  same log, PATH1 b047e87e19b02bd1, VU1 cycles 48338437, pictures byte for byte.
- **Modern against hosted, 1x / 4x** (pixels differing): herb 53; 009 195 / 2,047; 016 299 / 3,062; m8w 2,320 /
  33,985 and m9w 97,208 / 1,477,307 (T3's lighter reflection, as T3 measured); far save 617 / 4,499. On 000 (000,
  the spells, the fight, far: all on map 000) the counts are T5's sky at the eye against hosted's PS2 dome
  (000 133,103 / 2.1M, spells 267k / 2.1M, fight 190k / 1.5M; the heat maps show only the sky). With `--sky
  ps2` on both runs they are T1's to the pixel: 000 1,600 / 22,688, spell 3 3,146 / 22,385, fight 6,337 / 60,701.
- **The owner's spots** (bugs1022, bugs1024, `blink/door.kf4quick`; `title.pad`, frames 300 and 400) at 1x,
  against T3's build: with `--sky ps2` every 000 spot is T3's to the pixel (blotch 366, brazier 11,529, brick
  1,750, far 239, fire 343, lacing 13,859, lava 48, sign 3,042, sky2 0, teeth 242, tower 1,000); the others
  are T3's without it (pond 23,045, pool 7,626, shop 45, slow2 156, underwater 134,708, door 36). `slow`
  (099, a far-plane sky) is 83,068 against T3's 736: T5's sky in the scene, half a level brighter (T5's
  build alone gives 82,807). By default (the sky modern) the 000 spots differ in the sky only.
- **240** (every picture): far 619 / between 3,727 (T1: 618 / 3,725; 000's sky), fire009 324 / 1,785 and fight 6,424 / 2,991 as
  T1; water009 181,695 / 1,104,136 as T3; walk000 and spell 3 the sky at the eye. Every 240 run exited 0.
- **The composite** (GS 1x, scene 4x MSAA 4, against hosted 4x MSAA 4): herb, far and (with `--sky ps2`) the
  fight are T2's to the pixel; m9w is T2's plus T3's water tint (pixels off by more than 8: 9,897 / 4,137
  against T2's 6,991 / 3,261), the reflection in place.
- **States** (T6's `states.py`, 5 scenes, taken hosted and modern, loaded modern with and without the table):
  0 pixels between the table and `KF4_SA_NO_TABLE=1` in all 40 pictures; "1 tables loaded", 0 not uploaded.
- **`KF4_SA_VERIFY=1`** on m8w and m9w: textures 47,329 and 91,704 the same, CLUTs 47,327 and 91,702, 0 differ.
- **Exit codes.** Every modern run in these checks exited 0, the 240 runs included (T1-T3 saw 127 or
  0xC0000005/0xC0000374 at exit; T4's `sa.fetched` fix is in).

**Open after the merges** (from the task reports; owners in brackets): the scene's MSAA path costs about 0.7
ms more than the GS's, so the composite with `--scene-msaa 4` is above hosted's GPU time, and modern still
falls behind at 240 (T8); the reflection copied back to the GS at every vblank (T8); named targets at the GS's
scale and single-sampled (A9, T2/T3); the far-plane sky half a level brighter than hosted (Vulkan rounds, the
GS truncates); map 102's one row 2 levels off from the read-extent cut (T4); `host.h`'s `port_native_sky()`
comment still says 000 and 004 always get the PS2 dome; T7's review and the owner's sets; T8; Linux and
lavapipe; the 64-map tour in modern was not re-run on the merged build.

## T8: performance, and Linux (branch `p8/t8`, from `native/draw` 782ea5dd)

**Why modern fell behind at 240.** The GPU's own time a picture was low (2.0 ms at 4x), but `KF4_GSN_GPU_TIME` only adds
intervals within a command buffer. The native GS's thread waited 5-7 ms in Granite's `next_frame_context` fence after
every picture between, so `gsn_backlog()` stayed above 1 and gs.c gave the pictures between up for whole stretches
("all of 1,211 vblanks'"). The cause was one batch. In the far save, 3,392 triangles of map records reach far past the
widescreen frame's right edge, and 85 of them are in view. The batch blends in the shader (As above 0x80). In the frame,
`sa_draw_batch` drops the triangles outside the scissor first. In a picture between it kept all of them, because the GPU
moves them. Its runs (cut where triangles overlap) went from 1 to 117, and each run copied the whole scene: a full-screen
MSAA resolve, with the render pass ended around it.

**What changed** (`gsn_standalone.inc` only, no hooks):
- **Pictures between are culled as `flush_batch` culls them.** A native triangle outside the scissor is dropped when it
  is outside at t 0 too (`nd_box_at_0` plus its margin). The models', the strip's and the water's corners are kept
  (`nd_box_at_0` says "whole"), and so are the GS's own corners. A shader-blending run's copy covers its box (`run_box`,
  which takes t 0 into account) instead of the whole scene.
- **Shader blending copies the destination inside the render pass** (`flush_batch`'s COPY_IN_PASS). This applies to the
  scene's own corners (sa_000, sa_280 and sa_700 with `gs_in.frag` / `gs_in_ms.frag`). Each run's pixels go into the
  pass's scratch attachment (`pass_copy.frag`, or `pass_resolve.frag` when multisampled), and the run reads that
  attachment as an input attachment. Before, each run ended the pass, copied or resolved outside it, and began a new
  one: about 160 times a picture on map 009's poison pond (its miners and zombies, As above 0x80, in the frame and in
  the reflection). The scene's colour images now have the input-attachment usage. The GS's corners, the water,
  cut-outs shaded per sample and `--native-colour high` keep the copy. `KF4_SA_BLEND_COPY=1` (tests) brings the
  copies back for everything.
  - Single-sampled targets (the reflection) get exactly the same copy as before.
  - Multisampled targets are resolved by `pass_resolve.frag`: each channel's mean, rounded half up. NVIDIA's
    `vkCmdResolveImage` rounds a half either way, so a blended pixel can differ by 1-2 levels where its two samples
    differ by an odd amount. On the pond (3x, MSAA 2): 388 pixels in 2 pictures, worst 2.
- **Tried, and left out:** a depth-only program (an empty fragment shader) for a tie's Z-first draw when the alpha test
  cannot fail. It gave no measurable gain at the far save, because most Z-first draws there are cut-outs whose alpha
  can fail.

**GPU time at the owner's settings** (MSAA 2, native widescreen, cut-out AA, 240, `KF4_PRESENT_SIM=240`,
`KF4_GSN_GPU_TIME=1`, 1,400 vblanks from each quick save, the only kf4 on the PC, two runs each; the canyon's modern ranges take in two more runs of the culling change alone). Hosted is
`--renderer native --native-draw on`. Each cell gives ms a picture, then the pictures drawn (out of about 5,230 when a
run keeps up):

| save | scale | hosted | modern, 782ea5dd | modern, T8 |
|---|---|---|---|---|
| far (`quick_far`) | 3x | 2.24-2.26; 5,051-5,237 | | **1.82-1.83; 5,138-5,234** |
| far | 4x | 3.58; 5,237 | 2.00; **1,598** (pictures between given up) | **3.09; 5,228** |
| canyon (`quick_slow`) | 3x | 4.06; 3,899-4,124 | | **2.60-3.25; 4,454-5,033** |
| canyon | 4x | 4.86-6.11; 2,132-2,579 | | **4.08-5.47; 2,924-3,671** |
| pond (`quick_pond`, map 009) | 3x | 5.08-5.22; 1,713-1,727 | 6.53-6.54; 1,709-1,712 | **2.23; 1,709-1,712** |
| pond | 4x | 6.22-6.29; 1,679-1,682 | 7.35-7.37; 1,709 | **3.13; 1,709** |

- Modern is now faster than hosted everywhere, and it keeps up at 240 at the far save at 3x and 4x.
- The canyon falls behind at 240 in both modes. It is GPU-bound (42.8 million triangles in 1,400 vblanks at 4x, the
  tie's Z-first draws included), and its numbers vary from run to run with how far behind a run falls. Modern draws
  more of its pictures than hosted.
- The pond falls behind in both modes, and the GPU is not the limit there. The GS's thread spends about 9 ms a vblank on
  the frame's own work (GIF 12.8 s for 1,400 vblanks; hosted 15.5 s), mostly native draw's per-draw work
  (3,129 MB of meshes hashed).

**Pictures:**
- Modern T8 against modern 782ea5dd at the far save (4x, 240): the game frames differ in 1,974 pixels, worst 3 (the
  resolve rounding above). The pictures between differ in about 5,000 pixels, worst 9, along triangle seams and the
  right edge. With `KF4_SA_BLEND_COPY=1` it is 0 and 4,236 (worst 9): the culled pictures between are cut into other
  runs, and their blending order differs along the seams of overlapping triangles. That is the "risky runs" `flush_batch`
  has too.
- Herb modern (1x) is byte for byte 782ea5dd's, with the same log.

**Linux (gcc, WSL, lavapipe).** p8/t8 builds with gcc and `-DKF4_USE_SDL=OFF`, with no warnings in port/gs/native.
- The herb run hosted (`--native-draw on`) on lavapipe has the same log as Windows. Its pictures differ only by the
  sampler: worst 18, 61 pixels by more than 1 at vblank 1580.
- **`--renderer modern` on lavapipe is wrong, and it was already wrong at 782ea5dd.** The log, the PATH1 hash and every
  `[stats] standalone` count match Windows, and the `KF4_SA_LOG=2` logs are identical, but the pictures are not. From
  the map entry at about vblank 1552, the frame buffer the GS shows stops getting the scene's picture. It keeps the
  first frames' content: the clear colour over the sky, an older view of the ground below. The game's fade-in is drawn
  over it, so the difference grows from 3 levels at 1554 to all of the picture by 1800.
- What it is not:
  - It is not a race: `KF4_GSN_THREAD=0` and `LP_NUM_THREADS=1` give it too.
  - It is not the table (`KF4_SA_NO_TABLE=1`), the stencil (`KF4_SA_NO_STENCIL=1`), T3's named targets
    (`KF4_SA_NO_NAMED=1`), the sky (`--sky ps2`) or the pictures between (`--frame-rate 30`).
  - It happens with `KF4_SA_KINDS=0` too.
  - A one-vblank capture taken on Windows replays on Linux (`kf4-gsreplay --renderer modern`) to the same scanout. So a
    single vblank's commands give the same result on both, and the fault is in state carried from vblank to vblank.
  - A capture taken on Linux at 1600 carries the stale buffer: replayed on Windows it shows the same wrong picture.
- Next step: a validation layer, which neither machine has installed, or GS captures taken every vblank from 1550 on
  Linux, compared with Windows' captures VRAM page by VRAM page, to find the first page the scene's give-back does not
  reach.
- Running `gsn_draw_test`'s 300 cases on lavapipe took the WSL VM down (E_UNEXPECTED; it came back after a minute). The
  memory note says lavapipe keeps a process's memory. CI was left unchanged: a lavapipe step could not be checked here.

## T7 review, finding 1: a blended mesh over itself keeps every layer

**The bug.** A draw tested GEQUAL that writes Z (the tie rule) was drawn as its Z for the whole batch, then its
colour within the slack. The batch was cut into runs of triangles that do not overlap only for two alpha-test
passes or blending in the shader. So in a blended batch whose triangles overlap (the trees' foliage: ALPHA
`0x8000000044`, (Cs - Cd) As + Cd, TEST `0x5000d`, Z written), the back leaves failed against the front leaves'
Z and only the nearest layer was blended. The trees on 004 (looking up) and 013 (ahead) were thinned, the sky
showing through the leaves.

**The fix** (`sa_draw_batch`, `tie_runs`, as `flush_batch`'s): a tie that writes Z whose colour does not come out
the same drawn twice is drawn in runs of triangles that do not overlap, each run's Z then its colour. Not cut:
a skinned model's second pass (its triangles are its first pass's: drawn whole, only the nearest gets the shine,
as on the GS), and a cut-out whose every pixel the alpha test passes has As 0x80 (`sa_passing_same_twice`, from
the texture's alphas: blended, it is Cs).

**Pictures** (the owner's settings, still at 400; pixels differing from hosted, more than 8 in brackets):

| view | before | after |
|---|---|---|
| 004 looking up (`--sky ps2`, the geometry alone) | 312,394 (71,471) | 43,377 (5,757) |
| 004 looking up (modern sky: the clouds differ by design) | 1,820,728 (489,635) | 1,589,928 (425,788) |
| 013 ahead | 143,374 (31,498) | 18,822 (3,065) |
| 013 looking up | 240,235 (38,510) | 56,339 (6,301) |

The review's other sets (its spots, the sky maps, water, menus) are the same or fewer pixels off: brick 10,682
to 4,235 with `--sky ps2`, blotch 3,407 to 1,941, fire 4,973 to 3,858, underwater 1,302 to 543 more than 8;
014's water 33 pixels more (of 167,834). The seated man (finding 3) is unchanged: not this mechanism.

**Cost.** GPU time at the far save (4x, MSAA 2, native widescreen, 240, two runs each): 3.09 ms a picture before,
3.55 after; hosted 3.58. It still keeps up (5,228 pictures). The draws went from 425,650 to 1,110,866, but those
are about 0.08 ms of it (the same runs drawn in the old order: 3.17 ms). The rest is the back leaves now
shaded and blended, which the GS draws too: before, early Z threw them away.

**Finding 2** (009's water much lighter under `--scene-scale`) is not looked at yet.

## The owner's bugs at 78dd789c (branch `p8/owner-bugs`)

The owner played `native-78dd789` with `--renderer modern` at his settings (4x, native widescreen, MSAA 2,
cut-out AA, 240). Each was run headless at those settings (his `settings.json` in a fresh data folder), against
hosted (`--native-draw on`) and the PS2 path.

- **The rope bridge's light blocks are back** (`bugs1024/quick_bridge`, walking up to it). The scene's tie rule
  drew an opaque one-pass tie's Z first and then its colour within half a Z of that, so among a batch's own
  triangles a later one won within half a Z: a plank's lighter top faces over the faces in front of them, far
  off. Fixed as hosted fixed it (669bee0, `flush_batch`'s two_phase): such a tie (one pass, Z written, the same
  drawn twice; not a skinned model's second pass, not the shadows) draws its colour within the slack of what was
  there before the batch with no Z written, then the batch again Z tested and written exactly. Walking to the
  bridge, pixels off from hosted at 4x: about 2,300-3,000 a frame before, 950-1,700 after; the planks as hosted's.
- **Opening a menu or a shop flashed black** (any save; CIRCLE opens the menu). As the menu opens,
  `GameLoop_DrawFrameWithoutHud` draws the frame without the HUD and a brightness pass over it that reads the
  frame (`RD_PASS_SCALE`, colour 0xA0). The scene drew that pass from its copy of its colour; the scene's
  picture was right (read back), but the pictures between of that frame came out black, and so did the next
  scanout, which weaves them (406.25-407 at 240: 286,720 of 286,720 pixels off at 1x). A pass that reads its own
  frame is now the GS's when the frame is kept for pictures between (`interp.active`: declined, "reads its
  frame"), as hosted draws it. After: the open, the close and the shop's dialogue match hosted to the edges (the
  open's pictures between: about 4,400 pixels off by more than 32 at 4x, along silhouettes). Closing the menu
  showed nothing headless that hosted does not.
- **The campfire is missing in the tower** (`quick_campfire`) and **the locked chest by the pouch**
  (`slot0_chest`): not the renderer. Both are missing from the same saves in hosted, in the PS2 path and with
  `--renderer gs`. They are map objects their map has switched off: in the quick save's map flags
  (`map_entry_flags`, row 000) objects 37, 39 and 41 are inactive (an older quick save of the same room, bugs1022
  `quick_fire`, has them active and shows the fire in modern too); setting them active again in a copy of the
  save brings the campfire back, drawn by modern. The chest is a breakable container (`MapObject_Break`, definition
  bit 0x800): broken, it drops the item it held (`MapObject_UpdateBreakingItemHolder`), the pouch on the ground.
- **Not reproduced headless:** the walls that vanish at the screen's edges on the stairs (`quick_stairs`, turning
  both ways at lx 60 and 196), the shards in the doorway to the shop (`quick_doorway`, walking straight and
  weaving, at 240 and at 210 for other phases), the bug's noisy shell on map 63 (`quick_bug`, 1,400-1,800 with the
  bug in view, also with `KF4_PRESENT_SIM=240`, no `--fast`, every picture between drawn and `KF4_SA_VERIFY=2`:
  223,164 textures and CLUTs the same, 0 differ) and the shards on the shallow water (`quick_wade`, walking). In
  each, modern and hosted differ only along edges. On the stairs the one region off (about 16,000 pixels by more
  than 8 at 4x) is the torch light's edge on the right-hand wall, 10-20 levels, the same with the vertex depth
  left unclamped: not a missing wall. What is left to look at: an F9 capture (`.kf4gs`, which carries the table)
  or an F5 state taken while it shows, replayed with `kf4-gsreplay --renderer modern`.
- **Checks.** The review's sets again (its 19 spots, 4 water views and the menus, the owner's settings): every one
  the same or fewer pixels off hosted than after finding 1 (the far plane's sky, slow, 730 to 620 by more than 8 in
  game frames; the others within a few dozen). The off check: boot_test, frame_test gs and native PASS,
  gsn_draw_test 0 of 300 (seeds 1, 7, 1234), vu1_test 0 of 90; herb with `--native-draw off` against
  play-932801f: the same log, PATH1 204e443a9fc49731, VU1 cycles 460424273, pictures 1800/1820/1840 byte for byte.

### The doorway's shards, and the flash recorder (branch `p8/recorder`)

- **The owner's F9 captures** (8, walking through the doorway to the shop and back, modern at his settings)
  replayed with `kf4-gsreplay --renderer modern --scale 6,4 --msaa 2 --cutout-aa ssaa --layer-z tolerant`
  (`--frames`, and `--draws` for the targets at 4x): no shards in any game frame. A capture holds the game's
  packets, not the pictures between (frame_interp.c's moves are not in it), so it cannot show them.
- **Reproduced headless** from `quick_doorway` walking forward, every picture of vblanks 410-480 at 240
  (`--dump-frames` with `--dump-present 1280x720`): the shards fill the pictures between of one game frame
  (436_250 to 437_750) at the top and side edges, and the top-left goes the sky's grey, as in the owner's
  screenshot. Hosted shows the same; the PS2 path does not; `KF4_INTERP_STILL=1` and `--native-draw models`
  do not; the native corners not moved at all still do. The cause was frame_interp.c's pairing of the PS2
  path's runs: the map records' near-cut pieces (36-vertex strips, new batches each frame) were paired by
  order with the first look-alike of the frame before, its neighbour 120-380 units away when a piece came or
  went (RENDERER.md, "The near cut's pieces are paired by nearest, both ways"). Fixed there: 12 flagged
  pictures before, 0 after on the walk in, and on the stairs and wade walks the same as hosted.
- **Left (open):** walking backward out of the doorway, and turning on the stairs, a game frame's pictures
  between show the clear colour at a screen edge, the PS2 path too (RENDERER.md, "What the frame did not draw").
  Not modern's.
- **The flash recorder** (F8; README.md, "Debugging"): the last 2 seconds of presented pictures, a GS capture,
  a quick save and a save state into `captures/flash_*`; `tools/flash_find.py` finds the odd pictures. 5.4 us
  of GPU a picture at 240 at 4x; windowed A/B on the doorway walk, 4,853 pictures shown with it and 4,880
  without (nothing else running).

## Distance: layers by their real depth (branch `p8/distance`, from `native/draw` a1900776)

The owner played `native-a190077` with `--renderer modern` (4x, native widescreen, MSAA 2, 30 and 240) and met two
things that changed with the distance, both at 30 (the game's own frames):

- **The tower's top** (`bugs_modern/quick_tower2`, walking up to it): the rock over the top of the bricks changed shape
  and flickered as it grew. The rock is a layer (a mesh of its own, blended by its corners' alpha, GEQUAL, Z written)
  cut apart from the bricks': at its edge it is about 1.1 world units behind them along the view (`KF4_SA_PIXEL`: 9.6 Z
  at a depth of 350). The tie rule passed a layer within one Z plus the slope term. A Z is a different distance at each
  depth (Z = cz + az / depth, az about 1.05 million: a Z is depth^2 / az units, 0.04 at 200 and 8 at 3,000), so near
  the rock failed where it passed from afar, and its edge moved with every step. **The PS2 path and hosted do the
  same** (the GS's whole Z in a 16-bit buffer: a band of rock that grows and shrinks; the "patch on a tower's top" of
  `quick_brick` is the same thing).
- **The chest** (`states/slot0_chest2`, F5 with native-a190077; loaded once in that exe and quick-saved, so other builds
  load it): from afar half of its right end showed a lighter texture, which went away walking up. That is the chest's
  far end, a face of the same mesh 3.2 units behind its end panel along the view (1.9 Z at a depth of 1,300): within one
  Z plus the slope term, so from afar it came through. **Modern only**: hosted and the PS2 path keep it hidden. Not the
  mipmaps (the chest's TEX1 0x60, MXL 0, MMAG and MMIN both linear: `--native-lod scale`, `ps2` and `full` give the
  same pictures to the pixel) and not the table (it is the same with the GS's textures).

**The fix** (`gsn_standalone.inc`, "Ties"; `sa_slack` in `shaders/gs_common.h`, used by `sa_000`, `sa_280`, `sa_700`
and `sa_water`):
- A layer of native draw's corners passes within **two world units across its surface**, at every distance. The
  vertex shader has the scene's own depth: with q = 1 / depth, a depth change is (Z - cz) q times as much Z, and a
  surface whose Z changes g a GS pixel lies at tan t = g sx / (Z - cz) to the view. So e units across is
  e q sqrt((Z - cz)^2 + (g sx)^2) in Z. The triangle's g goes in its corners' alpha (32 log2(1 + g), the slope term's
  place); tan t is held to 1 (toward a silhouette e / cos t grows without end: with 3 a seam ran along a tower's top
  edge against the sky). One unit still left a notch of bricks in a corner of `quick_brick`'s tower; half a unit let the
  rock shrink walking up.
- **Exact over its own triangles.** A unit or two lets a model's faces behind its front through at its silhouettes
  (light seams along the logs of the tower's campfire, `quick_fire`), and the chest's far end is its own mesh. A layer
  drawn in runs (its triangles over its own) marks the stencil with a number of its own in each run's Z pass, where it
  wrote; its colour then passes exactly where it is marked (its own triangles) and by the world units elsewhere (the
  surface it lies on). The number is the shadows' counter (`sa.stencil_gen`; after 255 the frame is taken again to clear
  it). Not in a named target (the reflection) and not without a stencil: there the world units alone.
- The GS's own corners in the scene (`sa.gs3d`, the PS2 path's leftover map strips, gs.vert) keep the Z rule.
  `KF4_SA_WORLD=0` (tests) brings the Z rule back for every layer; `--stats` counts the layers exact over their own
  triangles.

**Pictures** (4x, MSAA 2, native widescreen, cut-out AA, 30; scratchpad `p8_dist/out/`):
- The tower walk (`tower_walk.jpg`: PS2 path | hosted | modern before | after, vblanks 500-548): the rock band stays
  one shape from the first picture to the last. Before, it was as the PS2's, a thin band that came and went. Against
  `KF4_SA_SLACK=64` (the rock everywhere, as a reference; from afar 64 Z is many units, so it is generous there):
  91,000 pixels off before at 460, growing to 463,000 at 540 walking up; after 74,000 at 460, falling to 9,000 at 540.
- The chest walk (`chest_walk.jpg`: hosted | before | after, 510-546): its right end is hosted's. Pixels off hosted
  around the chest (800 x 450 at 4x, more than 8 apart): 3,000-3,600 before from afar, 510-660 after at every distance
  (before 720-860 near).
- At 240 the pictures between follow their frames (24 pictures each from 520: hosted against after, no picture out of
  line).

**The review's spots** (T7's set: 3x, MSAA 2, native widescreen, cut-out AA, 240; pixels a game frame more than 8
off hosted, before -> after): far 682 -> 449, teeth 8,652 -> 8,696, pond 612 -> 519, brazier 31,694 -> 31,630, fire
1,151 -> 1,261, slow2 1,796 -> 1,824, underwater 543 -> 532, the water sets the same within 30; brick 412 -> 957 and
tower 1,296 -> 3,931 with `--sky ps2` (the rock now covering the patch hosted leaves); lacing 429 -> 617 and sign 474
-> 616 (the brazier stands' thin legs and a column's edge, a pixel's worth of which overlapping face wins); lava 167 ->
203; sky2 1 -> 60. At 4x against hosted (`--sky ps2`): fire 1,838 -> 1,978, slow2 2,916 -> 2,855, brick 745 -> 1,810.

**GPU time** (240, `KF4_PRESENT_SIM=240`, 1,500 vblanks, 4x MSAA 2, ms a picture, base -> after): far 3.54 -> 3.69,
the tower 2.70 -> 2.71, the fire 3.14 -> 3.18; every run kept up (5,730 pictures), turns 0.24 a picture as before.

### A skinned model's parts tie exactly (branch `p8/light-npc`, from `native/draw` f0871f86)

The owner played `native-f0871f8` with `--renderer modern` (4x, MSAA 2, native widescreen, 240) and saw, on the seated
men (`bugs_modern/quick_npc_cave`, by the brazier; `states/slot0_bridge_npc`, by the broken bridge): from afar the armour
on the arm and leg and the wrist flashing in front of the cloth over them, and a bright spot coming and going on the
neck and shoulder as the man rocks; close up it was right. Headless at 30 and at 240 it shows as speckles of hand, wrist
and shine over the cloth that change from frame to frame (`p8_light/out/cave_npc_440.jpg`, `bridge_npc_412.jpg`:
before | after | hosted). It is not p8/distance's: the build before it (fec275a) gives the same pictures.

- **Cause.** A skinned model's first pass (program 0x280: opaque, Z written, GEQUAL) is a one-pass tie, drawn by the
  two-phase rule with the tie's slack of half a Z. Its parts (armour, cloth, hands) are separate meshes a fraction of a
  unit apart, drawn one after another. A Z is depth^2 / az units: at the bridge the whole man is two or three Z deep, so
  half a Z is a good part of him, and a part drawn after the cloth and lying under it came through. Its second pass (the
  sphere-mapped shine, exact on its first pass's Z) then lit the spots where it did: the bright spot on the neck.
- **The fix** (`sa_draw_batch`): a batch of program 0x280's corners other than the second pass ties by its real depth,
  exactly (`KF4_SA_SKIN=EPS`, tests: within EPS world units along the view; a blended skinned layer the same, across its
  surface). Exact was nearest hosted at every distance tried. 0.05-0.3 units were a little worse from afar, and close up
  (the shopkeeper, `quick_shop`) even 0.1 units let his shirt through along his shoulder: pixels more than 8 off hosted
  at 4x, 730 before, 429 exact, 1,369 at 0.1, 4,158 at 0.5.
- **Numbers** (4x, MSAA 2, native widescreen, cut-out AA, 30; pixels a frame more than 24 off hosted around the man):
  the bridge, walking away (`quick_bridge` made from the state), 50.7 before, 14.5 after; the cave, walking up to him
  (a 1000 x 800 box with the wall), 315 before, 254 after. The review's spots (T7's set, 3x, 240; pixels a game frame
  more than 8 off hosted, `--sky ps2`): shop 409 -> 267, far 458 -> 402, bridge 1,193 -> 1,120, lacing 617 -> 539,
  pond 516 -> 476, brazier 476 -> 422, m9w 898 -> 767, door 70 -> 73 (3 pixels along an edge); every other spot and
  water view to the pixel. The tower (`quick_tower2`) and the chest (`chest2`) are p8/distance's to the pixel.
- **Not reproduced: the dark patch on the lit wall** (the cave, images 122 and 123: a dark rectangle on the wall by the
  man's head, and squares flashing over the lit walls walking or turning). Not in any headless run: idle and walking up
  at 30 and at 240 (every picture, `flash_find.py`), turning both ways at two places, with his `settings.json` read
  (`KF4_OVERLAY_SHOT` makes a headless run read it), without `--fast` and with `KF4_PRESENT_SIM=240`; modern against
  hosted shows no dense block in any of them. Ruled out headless: the stencil's wrap (the frame taken again every 3
  numbers instead of 255 gives the same pictures to the pixel, at 30 and at 240), the table (`KF4_SA_VERIFY=2` after the
  quick load and the state: 0 differ), the man's own parts (any skinned slack up to 3 Z draws nothing on the wall), and
  the shadows (the shadow mesh drawn by the GS instead changes nothing on the wall). An F8 recording or an F9 capture
  taken while it shows is what is needed next.
### The lamp wall's patch: a picture between's depth from the GS's Z (branch `p8/lamp-wall2`, from `native/draw` 9ef866f5)

The owner, `native-c6ef727` with `--renderer modern` at 240 (`bugs_modern/quick_lampwall`, the cave with the brazier and
the seated man, seen small at the right of the screen from the passage): dark patches on the lit wall by the man's head,
a quarter darker than the wall, only above 30 and only from afar; also by other lights. It was taken for a shadow
(program 0x5A0) moved wrongly, but it is not one, and not frame_interp.c's pairing: modern has no PS2-path runs of the
world there (the outside check below logs none).

- **Found** with the flash recorder over a whole windowed run (`--flash-recorder 8,540`, F8 at the run's end via
  `KF4_DEBUG_AT=flash@V`) and a detector of pictures darker than both neighbours. The patch came in the pictures the
  window showed when the native GS fell behind (the vblanks' own pictures only: there the neighbours are game frames);
  at full rate it is in every picture between of the frame alike, so a neighbour test does not see it. Then headless,
  every vblank: `quick_lampwall`, `100 CROSS 4`, `--frame-rate 240` (or 60), vblanks 218-219: every picture between of the
  frame drawn at 218 has a wedge on the wall from the brazier down toward the rock (121 pixels at 720 x 540), the game
  frames not; also at 343, 345, 585, 829 (6-120 pixels). Not with `KF4_INTERP_STILL=1`, not hosted, not with
  `KF4_INTERP_CPU=1`; the same with `KF4_SA_NO_STENCIL=1` and with the shadows not native.
- **Cause.** In a picture between the scene's corners (sa_000, sa_280, sa_700, sa_water: `moved` in nd_000.vert and the
  others, ND_STANDALONE) took their depth from the moved GS Z, `float(r.z)`, which is VU1's Z after FTOI4 and the shift
  to whole Z: up to a Z farther than the frame's own, unrounded `zf`. Near, a Z is a hair; from afar many world units
  (p8/distance: depth^2 / az, 8 units at 3,000). The light on the wall is a layer tied to the wall by its real depth,
  and where its corners lay a Z farther it failed its test: the wall without its light, the patch. The camera need not
  move at all (here it moved 1e-8).
- **Fix.** The scene's depth moved from its own unrounded Z: `zf - (zf - cz) dm.z / mz`, as the GS's Z is moved
  (`gsn_shaders.h` rebuilt: the four sa_ shaders change). The game's frames are unchanged (byte for byte); hosted's
  pictures between too (its Z is the GS's).
- **Numbers** (headless 240, 720 x 540, pixels of a picture between more than 15% darker than both its game frames,
  vblanks 216-222, 340-346, 582-588, 826-832): 448 before, 0 after. Windowed with `KF4_GSN_SLOW_US=12000` (the window
  behind, about 980 pictures of 1,920 the vblank's own), the patch in 3, 3, 4 and 5 pictures of four runs before; 0, 0
  after; a run at full rate walking, 0. The doorway walk (`quick_doorway`, 410-480 at 240, 1280 x 720): `flash_find.py`
  0 flagged before and after, with and without `--frames`; the game frames the same bytes, the pictures between within
  a few dozen pixels (edges) of before.
- **Shadows to the GS are slow** in modern (`--native-draw` without `shadows`), as the owner found: each run of shadows
  then is the GS's, between turns of the scene (the frame given back and taken again), several a frame. Expected; the
  default draws them in the scene.
- **Tools** (README.md, "Higher frame rates"): `KF4_INTERP_OUTSIDE` (the PS2 path's runs whose box in a picture between
  leaves their boxes at t 0 and t 1; it does not see native draws, which is why it found nothing here) and
  `KF4_GSN_SLOW_US` (a window behind, on purpose).

## 240 at the owner's spots (branch `p8/perf240`, from `native/draw` 5111e87d)

The owner, `--renderer modern` at 240 (3x, native widescreen, MSAA 2, aniso 2, cut-out AA ssaa, Vulkan presenter,
2560 x 1440, RTX 5080): even at 30, but an almost solid 60 while looking at the pond (`bugs1024/quick_pond`), the
fortress entrance with the two braziers (`bugs_modern/quick_fortress`), the canyon with the beetles
(`bugs1024/quick_slow`), and under water (`bugs1024/quick_underwater`, walking forward 60 vblanks).

**Why exactly 60.** It was fix/underwater's rest: with the native GS's thread a vblank behind at two vblanks in a row,
gs.c took every picture between away for half a second, then 1, 2 and 4 s while it kept falling behind. Then only the
vblanks' own pictures were left: 60. At these spots the rest was on for 1,000-1,230 of 1,400 vblanks.

**What still costs the GS's thread** (the pond, a game frame, measured with scoped timers): the frame's packets about
20 ms, the picture between drawn from the packets (the recording) 13-16 ms more, each replay 1-4 ms. At 240 that is
more than the 33 ms of a game frame. In the frame:
- the skinned models' draws (program 0x280: the zombies, miners and the bugs) about 10 ms with their batches. Each part's
  batch writes its own TEX0 and A+D registers, so each is a batch of its own: about 800 batches and 3,500 Vulkan draws a
  game frame (Granite records each one: about 3 ms);
- `sa_draw_batch`'s own work about 4.7 ms: culling, the runs for shader blending and ties, the corners' slopes;
- the CPU plan of the skinned vertices (VU1's float arithmetic, `nd280_plan`) about 2 ms;
- uploads about 2.6 ms (6 MB a game frame, each hashed and its blocks owned in the table);
- the CLUT loads (the GS's and the table's mirror, at every TEX0) about 2 ms;
- the draws' set-up (`nd_const`) about 1 ms.
The recording does all of that again, for the GPU's replays. Under water the picture between also wrote the frame, its
Z buffer and the wobble's copy back to VRAM (three read-backs a picture, each waiting for the GPU) and decoded the frame
at 1x. The canyon is GPU-bound as T8 found (5.3 ms of GPU a picture at 3x).

Not costs: the mesh hashing (`RD_GEN_HASH`, 3,129 MB on a 1,400-vblank pond run) is on the game's thread, about 0.5 ms a
game frame of a thread busy about 25%. `sa_draw_begin`'s second `rd_fetch` copies one draw (under 1 us). The flash
recorder is one blit a picture on the GPU. `KF4_SA_VERIFY`, the logs and `KF4_GSN_GPU_TIME` are off by default. The DMA
controller's thread shows 30-45% of a core, most of it spinning while it waits (`nap`, `lock_queue`, `claim_starts`).

**What changed:**
- **The fallback cuts, it does not snap.** A vblank behind takes one picture between a vblank away (3 to 2 to 1 to
  none at 240), not all of them. It waits up to half a second for the thread to catch up before cutting again, and
  gives one back after half a second without falling behind (after twice as long when that falls behind again at once,
  up to 8 s). The pictures kept are spread over the vblank. `--stats` says how often it cut and how many vblanks were
  drawn with fewer and with none. The F3 counter's second line reads `30 GAME CUT 2` while it is on.
- **Under water: no write-backs a picture.** A GPU copy of the frame over whole pages of a 32-bit buffer (the wobble's
  copy to 0x2BC0) drops the targets wholly under it without writing them back, makes its target without loading VRAM,
  and lets go of the shadows' pages under it (`overwrite_lo`/`overwrite_count` in gsn.cpp; `--stats`: "write-backs not
  made"). A texture whose V is less than a row above its top reads the target (`rows_read`): the other field's tint
  pass (moved up half a line) took the whole 1024 x 512 frame as read, sent the frame and its Z buffer back to VRAM, and
  drew the frame decoded from VRAM at 1x. That was also every field picture under water at 30 (blurred at 1x): these
  game frames now differ from before, sharp at the scale.
- **Kept instead of worked out again:** the table's owners of a rectangle while the table is unchanged (`sa_owners`,
  `sat.gen`: 94% found again on the pond); a skinned batch's plan for the same matrices (the recording, the parts
  standing still); `nd_const`'s VU arithmetic by its inputs; a CLUT load the same as the last with its pages unwritten.
  The CLUT is read a row at a time (`clut_read_entries`).

**Numbers** (headless, `KF4_PRESENT_SIM=240`, 1,400 vblanks, 760 under water; pictures shown a second, two runs;
windowed at the owner's settings, 1280 x 720, the same within a few):

| spot | before | after | after: the cut | GPU a picture |
|---|---|---|---|---|
| pond | 67 (rest 1,183 vblanks) | 68-69 | 9 cuts, none in 1,080 vblanks | 3.0 ms |
| fortress | 61 (rest 1,226) | 72-76 | 10-17 cuts, fewer in 1,150, none in 240-630 | 3.7-4.5 ms |
| canyon | 64-70 (rest 1,000) | 119-128 | 7-8 cuts, fewer in 190-350, none in 0 | 5.3 ms |
| under water | 93 (rest 470) | 202 | 1 cut (30 vblanks) | 1.9 ms |

The GS's thread (CPU, % of a core, the same runs with a sampler): pond 81% before, 69% after (both mostly without
pictures between); fortress 54/56%; canyon 40/86% (drawing three times the pictures); under water 44/60%.
Under water now keeps up (about 240 once submerged). The canyon is GPU-bound. The pond and the fortress still fall
back, now to as many pictures as fit (the fortress 1-2 a vblank, the pond none: its frame and recording are about
35 ms of the GS's thread a game frame). What would move them: the skinned models' batches drawn as one (a texture
array or bindless textures, so a part's TEX0 is not a batch of its own), their plan on the GPU (the clip and ADC
decisions in a compute pass), and the recording not drawn from the packets again (recording the frame's own draws).

**Pictures:** game frames byte for byte at the pond, the fortress and the canyon (vblanks 200, 301 and 399 at 240,
`--fast`, every picture); under water the game frames at vblanks 400 and 600 the same, and the field pictures (501)
sharp where they were blurred (above). The off check passes: boot_test; frame_test `--renderer gs` and `native`;
gsn_draw_test 0 of 300; vu1_test 0 of 90; herb with `--native-draw off` against play-932801f: the same `--log`, PATH1
hash 204e443a9fc49731, VU1 cycles 460424273, pictures 1800, 1820 and 1840 byte for byte.

### A skinned model's parts drawn together (branch `pf/skin`, from `native/draw` 5abdb02a)

The skinned models' cost at the pond and the fortress, from "240 at the owner's spots" above.

**What separated the parts was not their texture.** Program 0x280 kicks a model a part at a time (an RdBatch, at most
40 vertices): its first pass (opaque, Z written, GEQUAL, the part's TEX0), then its sphere-mapped second pass, the shine
(another TEX0, ALPHA `Cs + Cd As`). The shine's TEX0 and ALPHA end the first pass's batch, and the next part's ALPHA
ends the shine's, so every part was two batches of its own. The first passes mostly share their state: of each first
pass, the one before had the same TEX0 81% of the time at the pond (the zombies), 97% at the fortress. A texture
index would not have joined them; the shine between them would still.

**What changed** (`gsn_standalone.inc`, "A skinned model's parts drawn together"):
- **The parts kept and drawn together.** Each batch is still worked out as before (`sa_draw_batch`: the texture, the
  alpha range, the ties, the runs, so the decisions stay each part's own). A part whose draws are the simple ones (a
  first pass two-phase in one run, exact, no pixel of it failing the alpha test; its shine in one run) is kept, from
  draw to draw of the model, and drawn when anything else comes (`sa_group_draw`: another batch or draw, the scene's
  turn ending, a picture between's mark, a save state): the first passes in one draw for each of their two draws,
  then the shines, last part first. The same picture, exactly:
  - the first passes together: the colour within no slack, then Z tested and written, over all of them, leaves what
    each part's two draws one after another leave (the second draw writes, last, every pixel the first did);
  - a part's shine stays, drawn in turn, only where its own first pass was the last to draw (a later part over it
    overwrote it), on that first pass's colour. So each shine is drawn where its first pass is at the front (its Z,
    exactly) and no later part's is: after its shine a part marks its front pixels in the stencil with the group's
    number (a number of its own, as the shadows' runs and the layers have), and a shine is not drawn where the
    stencil has it. Only a part whose screen box meets that of an earlier part with a shine needs its mark (in a
    picture between, every one: the corners move on the GPU), and the shines between needed marks are one draw.
    Without the marks, 13 pixels of a frame at the pond were a few levels off: a later part's pixel at exactly an earlier
    part's depth (the edge two parts' triangles share where they fold) took the earlier part's shine too. The
    shines' own Z pass is left out: it writes the Z its first pass wrote there.
  - A named target's turns (the pond's reflection, where 45% of the zombies' batches are) start the stencil's numbers
    at 0 again without clearing it; a group there takes a number above those of its earlier turns
    (`sat_stencil_number`, `SatTarget::stencil_used`).
- **The plan with the host's floats** (`gsn_nd_280.inc`, `nd280_plan_fast`). VU1's arithmetic is IEEE's rounded
  toward zero with denormals read and written as zero (vu_float.h), so SSE with MXCSR set so (round toward zero,
  DAZ, FTZ) gives VU1's bits, four lanes at a time. Where VU1's rules differ (a value with exponent 255, its largest
  float, a divisor with exponent 0, FTOI past the integers) any value with exponent 254 or more, read or made, sends
  the batch to `nd280_plan` (1.2% of them at the pond). `KF4_ND280_PLAN_CHECK=1` plans every batch both ways: 0 of
  388,000 differed at the pond and the fortress. Clang on x86-64 only (`#pragma STDC FENV_ACCESS ON`); elsewhere the
  old plan. 0.92 s to 0.09 s of a 600-vblank pond run.
- **The CLUT loads** (`clut_load`, `sa_clut_load`): the parts load the first pass's CLUT and the shine's in turn, so
  "the same load as the last" never held. An 8-bit CLUT of 32-bit colours loads the whole buffer, whatever it held:
  the last four such loads are kept (their pages not written since; the table's while it is unchanged, `sat.gen`), and
  the mirror's whole load is written at once. 1.6 s to 0.3 s of a 1,400-vblank pond run.
- `sa_draw_batch`'s corners and runs keep their room from batch to batch (globals: a function's static made on the
  GS's thread is gone at the exit while it draws); a run after its in-pass copy keeps the textures, CLUT, corners and
  buffers the run before bound (`pass_serial`: a binding lasts to its render pass's end).

**Numbers.** `--fast`, 240, the owner's settings, 600 vblanks (150 game frames, each drawn, recorded and replayed
three times); the GS's thread ("GIF"), two runs each:

| spot | GS thread a game frame | Vulkan draws | 0x280 batches drawn on their own, a pass |
|---|---|---|---|
| pond | 55.5 -> 37.7 ms | 1,701,432 -> 1,412,559 | 1,351 -> 388 (83% kept, 7 a group) |
| fortress | 41.1 -> 31.2 ms | 2,185,064 -> 2,035,241 | 747 -> 249 (75% kept, 9 a group) |

A kept batch drew 2 draws before, 1.14 now (the first passes' two a group, a shine and a mark a part). What is
not kept at the pond is the blended parts (their alpha from 0 to 0x80, drawn in runs blending in the shader, 7% of the
0x280 batches, about 3 ms a pass) and their shines.

Real time (`KF4_PRESENT_SIM=240`, 1,400 vblanks, 760 under water; one game at a time, two runs; pictures shown a
second):

| spot | before | after |
|---|---|---|
| pond | 67.6, 68.7 (none in 1,038-1,073 vblanks) | 97.2, 81.3 (none in 0-360) |
| fortress | 112.0, 118.4 | 158.9, 136.5 |
| canyon | 151.7, 149.6 | 168.3, 166.4 |
| under water | 202.9, 202.8 | 201.5, 202.6 |

Two games at once halve these (the GPU shared): the pond 63 -> 70, the fortress 62 -> 115 that way. The pond still
cuts: its frame is about 19 ms of the GS's thread and its recording as much again. The pictures between it now draws
cost the GPU too: 3.2 -> 8 ms a picture shown, as each recording draws the frame again on the GPU (recording the
frame's own draws would take both away).

**Pictures:** byte for byte, every picture (3 game frames and their 9 pictures between, vblanks 200, 301, 399 at 240,
`--fast`; under water 400, 501, 600) at the pond, the fortress, the canyon and under water, and at the shop, the
bridge, the cave's seated man, the bug and the brazier; the herb in modern at 1x and at the owner's settings. The off
check passes: boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test
0 of 90; herb with `--native-draw off` against play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles
460424273, pictures 1800, 1820 and 1840 byte for byte.

**Not done:** a texture index for the first passes (Granite has bindless sets and arrays of textures; the RTX 5080 and
the AMD iGPU index sampled-image arrays non-uniformly): at least 20,000 of the 47,315 groups of the 600-vblank pond
run ended on a texture alone, two draws each; not worth a shader of its own yet. The skinned plan on the GPU (the clip and ADC
decisions in a compute pass): with the host's floats it is 0.3 ms a pass.

### The recording and the replays (branch `pf/record`, from `native/draw` 5abdb02a)

The task: the first picture between of a game frame walks the frame's packets again to record them (13-16 ms of the
GS's thread at the pond), and each picture after it makes the recorded calls again (1-4 ms). The aim was a recording
made by the frame's own draws, so that the first picture is a replay too.

**What the recording costs** (the pond, `--fast`, sampled in `interp_record` only): the native draws 74% (the skinned
models' `nd280_draw` 45%, their plan 13% of it: `nd280_plan_cached` keeps two plans a batch, the world's and the
reflection's, so zombies sharing a mesh plan again), `sa_draw_batch` 41% (Granite's calls 15%), the CLUT mirror 12%,
uploads asked for again 7%, the boxes at t 0 (`nd_box_at_0`) 6%. It is the frame's own batch work over again: what
pf/skin takes out of the frame comes out of the recording too.

**Why the frame's own draws are not the picture's recording.** They differ in what is drawn and in what was decided:
- the packets: frame_interp.c's REPLAY walk draws into the buffer shown, leaves out the uploads the GS has (asked for
  again), marks the frame shown blended in (0xF2, 0xF4, 0xF5) and puts the registers back at its end;
- the moves: each kick's `GsnInterpMove` and the camera at t 0 (`move0`, `mid0`) are made at the vblank after the
  frame, by the walk that pairs it with the frame before; the GS has drawn the frame by then;
- the decisions made for both ends: a triangle outside the scissor left out only when outside at t 0 too
  (`nd_box_at_0`, from the camera at t 0), shader blending's copies covering both ends (`run_box`), the rows drawn
  (the whole scissor), the alpha range of both frames (`interp_alpha_range`), the texture read whole (`read_extent`),
  a pass reading its own frame declined, a native batch never joining a PS2-path batch (`nd_draw`'s flush), the
  records' moves (`nd_model_flags` against `nd_remember`), the GS's own 3D from `gs_replay_corners.comp`.
The frame's draws decided at t 1 alone leave out triangles a picture shows at the screen's edges while turning, and
blend from copies that do not cover where the moved triangles are. Taking the picture's decisions in the frame
changes the frame's batches (with MSAA, runs split elsewhere change pixels at shared edges: STEP3.md, "Silhouettes
in runs"), and those decisions are inside the batch drawing pf/skin is changing. Two ways remain, both larger than
this task: the frame drawn from the recording at t 1 (the walk and its moves before the GS draws the frame, and the
picture at t 1 the frame byte for byte), or the scene's batches drawn twice as the frame comes, a picture's state of
the scene kept beside the frame's (its pass, its acquire, its stencil numbers, the records with moves in a buffer
of their own at the same numbers), the picture's stream then walked only for its uploads, CLUT loads and the GS's
2D: by the profile above, a third or more of the recording would remain (the uploads, the CLUT loads, the registers).

**What changed** (the command layer, `Cmd`, and `replay_ops`; every draw the same):
- **The draws' data in chunks of our own.** Vertex and constant data go into append-only buffers (8 MB and 2 MB,
  `DataChunk`), not Granite's 4 KB vertex blocks, of which a batch of more made a Vulkan buffer of its own each time.
  A recording keeps its chunks: its calls made again bind the data where it is instead of copying it into Granite's
  memory each picture. A CLUT the same as one before it in the recording is bound where that one is
  (`same_constants`), and not bound again when it is bound already.
- **A program set when a draw needs it.** Each scene batch sets the map's program and then its kind's
  (`sa_draw_range`); Granite binds the pipeline again for any program set, the same pipeline or not. A recording at
  the pond had 3,620 program changes, now 1,142.
- **The same storage buffer not bound again** (native draw's arena and records, each batch): Granite writes the
  set's descriptors again for any buffer bound. A recording had 6,011 such bindings, now 22. `Cmd` forgets what is
  bound where Granite does (a render pass's beginning and end, a new command buffer) and where something binds past
  it (the kicks' pass, `replay_ops`, a recording's beginning and end).
- **--stats:** why a picture was drawn from the packets again, apart: the targets, the registers, VRAM or the CLUT
  written, a recording that cannot be made again; and the replays that named a target the recording made (kept, or
  made and dropped: its own). The canyon's sky copy: every replay names one kept.

**What a replay still costs:** a pipeline bound for almost every draw (the pond: 1,205 binds for 1,213 draws a replay;
the fortress 4,035 for 4,047): the tie rule's Z-first and colour draws differ in depth write and colour mask, which
are pipeline state in Granite. `vkCmdBindPipeline` is 37-44% of a replay's CPU. Vulkan's dynamic state for depth
write, compare, the colour mask, blending and the stencil (extended dynamic state 1 and 3) in Granite would make
them one pipeline: a Granite patch, for the frame, the recording and the replays alike.

**Numbers** (headless, `KF4_PRESENT_SIM=240`, owner's settings, 1,400 vblanks, 760 under water; two runs each, base
5abdb02a; another game was running on the PC, so the pictures a second move with the fallback's cuts; GS ms are the
GS's thread a game frame, from `--stats`):

| spot | pictures a second | frame's own GS ms | recording ms | replay ms | vblanks with none between |
|---|---|---|---|---|---|
| pond | 67-68 / 66-68 | 19.3-19.7 / 18.1-19.5 | 14.1-14.3 / 13.8-14.0 | 1.06-1.14 / 0.72-0.75 | 1,092-1,098 / 1,086-1,094 |
| fortress | 63-65 / 67-78 | 13.5-14.4 / 11.8-12.4 | 15.4-15.6 / 16.2-16.7 | 3.31-3.73 / 2.31-2.38 | 1,022-1,048 / 0-300 |
| canyon | 88-107 / 115-132 | 14.2-14.5 / 12.5-13.5 | 10.9-11.5 / 9.9-10.7 | 1.60-1.63 / 0.97-1.04 | 0-120 / 0 |
| under water | 133-202 / 186-201 | 7.2-9.7 / 7.0-7.6 | 7.6-8.2 / 7.5-7.6 | 0.27 / 0.22-0.23 | 0-60 / 0 |

The GS's thread (sampled, % of a core): pond 70/68, fortress 52/76 (drawing 2.3 times the replays), canyon 69/87
(2.9 times), under water 28/27. The pond is still the frame (about 19 ms) and the recording (about 14 ms) a game
frame: no room for a replay. With pf/skin's batches both shrink.

**Pictures:** every picture the same as 5abdb02a's, frames and pictures between, at the four spots (`--fast`, vblanks
200-207, 301, 399; under water 400-407, 600), the pond hosted (`--renderer native`) and walking on map 000 at 240
(`walk.pad`, 1890-1906). The off check passes: boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test
0 of 300; vu1_test 0 of 90; herb with `--native-draw off` against play-932801f: the same `--log`, PATH1 hash
204e443a9fc49731, VU1 cycles 460424273, pictures 1800, 1820 and 1840 byte for byte.

### Both merged (`native/draw`, pf/skin then pf/record on 5abdb02a)

The two merged without a conflict in the code (only this file's two sections). Every draw of pf/skin's groups goes
through `Cmd`, so pf/record's lazy program and its storage-binding check cover them; the groups' own kept bindings
(`pass_serial`) are skipped calls, not calls past `Cmd`.

**Pictures:** every picture the same as 5abdb02a's, game frames and pictures between: the four spots (`--fast`, 240,
the owner's settings, vblanks 200-207, 301, 399; under water 400-407, 600: 156 pictures, 0 differ); the herb run in
modern at the owner's settings (the same `--log`, PATH1 hash 8b78aa0191adbac9, pictures 1800, 1820, 1840). The off
check passes: boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test
0 of 90; herb with `--native-draw off` against play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles
460424273, pictures 1800, 1820 and 1840 byte for byte.

**The GS's thread a game frame** (`--fast`, 240, the owner's settings, 600 vblanks, two runs, 5abdb02a -> merged; the
frame's own is the GIF time less the pictures between; each game frame there has one picture between, recorded from
the packets; under water's figures carry the loading):

| spot | in all | the frame's own | a recording | Vulkan draws |
|---|---|---|---|---|
| pond | 40.0 -> 26.6 ms | 20.2 -> 14.3 ms | 19.7 -> 12.3 ms | 1,701,432 -> 1,412,559 |
| fortress | 30.4 -> 21.4 ms | 13.8 -> 10.0 ms | 16.5 -> 11.4 ms | 2,185,064 -> 2,035,241 |
| canyon | 26.2 -> 21.7 ms | 16.0 -> 13.0 ms | 10.2 -> 8.7 ms | 1,227,654 -> 1,081,744 |
| under water | 21.3 -> 21.4 ms | 14.6 -> 14.0 ms | 6.7 -> 7.4 ms | 63,062 -> 63,062 |

**Real time** (`KF4_PRESENT_SIM=240`, 1,400 vblanks, 760 under water; pictures handed over a second of wall time,
loading included; one game at a time). The owner was playing another game on the GPU (97% busy) during these runs:
of four rounds only the second had the GPU to itself for most of each run; the others drew about 58 a second at every
spot, base and merged alike, at 9 ms of GPU a picture. The second round:

| spot | pictures a second | cut to fewer (vblanks with fewer, with none) | a replay | GPU a picture |
|---|---|---|---|---|
| pond | 58.8 -> 80.9 | 8 (1,271, 1,184) -> 13 (1,207, 360) | 3.1 -> 1.7 ms | 5.1 -> 8.1 ms |
| fortress | 103.2 -> 139.5 | 13 (396, 0) -> 3 (97, 0) | 3.7 -> 1.8 ms | 5.8 -> 6.1 ms |
| canyon | 148.9 -> 165.3 | 4 (180, 0) -> 1 (30, 0) | 1.4 -> 0.7 ms | 5.6 -> 5.2 ms |
| under water | 202.2 -> 203.0 | 1 (30, 0) -> 1 (30, 0) | 0.22 -> 0.18 ms | 2.0 -> 1.9 ms |

The pond now draws pictures between (520 replays against 13), so its GPU a picture shown rises: each recording draws
the frame again on the GPU. What is left at the pond is the frame (about 14 ms) and its recording (about 12 ms) on the
GS's thread, and the GPU: the recording made from the frame's own draws (pf/record's two routes above) would take
both.

### The frame's own work kept for its first picture between (branch `rc/own`, from `native/draw` d715daed)

Job 2 of "240 at the owner's spots": the first picture between of a game frame walks the frame's packets again
(`interp_record`), working out every draw and batch the frame has just worked out.

**Which of pf/record's two routes.** Neither can be made exact as written:
- (a) the frame drawn from the recording at t 1: the picture's decisions are not the frame's. Its triangles outside the
  scissor at t 1 but inside at t 0 are kept, and with them the runs cut elsewhere (with MSAA, pixels at shared edges
  change: STEP3.md, "Silhouettes in runs"); the GS's own 3D and 2D corners are made on the GPU
  (`gs_replay_corners.comp`, not bit for bit the CPU's: STEP3.md, "On the GPU"); a pass reading its own frame is
  declined; the rows given back are the whole scissor. The frame at t 1 would not be d715daed's frame.
- (b) the picture's version of each scene batch recorded as the frame draws it: its culling and copy boxes need the
  camera at t 0 (`move0`, `mid0`), which frame_interp.c makes only at the vblank; and the calls it makes depend on the
  picture's state of the scene at that point (its turns with the GS, the render pass, the stencil numbers, the
  skinned groups kept), which the frame does not have. Recorded ahead, every one would have to be checked against
  the picture's state anyway.

What is done instead is the part of (b) that is exact by construction: what the picture works out **from the same
inputs** as the frame is kept by the frame and taken by the picture. The picture still walks the packets (the
uploads asked for again, the CLUT loads, the registers, the GS's own batches, the scene's turns and its calls into
`Cmd`), but no longer works the draws and the scene batches out again.

**What is kept** (`gsn_nd_prep.inc`; `sa_memo_*` in `gsn_standalone.inc`):
- **A draw's record and its batches' corners.** `nd_draw`, `nd280_draw` and the water's `ndw_draw` keep, for each draw
  (by its place among the frame's native draws, and its value), the record as the set-up made it (VU1's set-up:
  `nd_const`, `nd280_const`, the water's vertex matrix) and each batch's corners as the plan made them (with the
  mesh, the batch, the pass, PRIM, the mesh's place in the arena, and the water's alpha and XYOFFSET). The picture
  takes the record (its moves are its own, worked out as before) and copies the corners, the record number in them
  changed to the picture's (bits 7-21 of the RGB, which nothing else shares). The plans' own caches keep two a batch,
  so a mesh drawn more than twice (the zombies) was planned again for the picture.
- **What a scene batch works out from its corners alone.** For a batch whose corners are a run of the frame's kept
  corners and nothing else (`nd_prep_run`), `sa_draw_batch` keeps its corners' alphas and rows, whether it is a second
  pass or a skinned model's, which triangles are outside the scissor, where its triangles overlap (the runs), each
  triangle's slope, and whether every corner is native. The picture's batch of the same run (and scissor) takes them.
  The triangles the frame left out are tested at t 0 as before (`nd_box_at_0`): when one is kept there, the picture
  works the batch out itself. Its texture, blending, ties and calls are worked out as before.
- **The runs' overlap test** takes each triangle's box once a batch, not once a pair (the same floats and test as
  `triangles_overlap`'s first one): in the frame too.
- Kept only while frames have pictures between drawn from the packets (none for 8 frames: nothing kept), so 30 pays
  nothing; let go with the GS's uploads (`forget_uploads`: a state loaded).

**Every case that works out again is counted** (`--stats`, "native draw, pictures between" and "standalone, pictures
between"): no frame kept (the first picture after the pictures begin), another draw at the place, another program;
a batch of another mesh, PRIM or arena place; a scene batch not of kept corners alone, none kept (the frame left all
of it out: off the screen at t 1), another batch of those corners, a triangle left out kept. The pond's 600 vblanks:
62,370 draws' records and 462,285 batches (14.4 million corners) taken, 201 draws worked out again; 74,267 scene
batches took the frame's work, 31,550 found none (the zombies' parts off the screen at t 1, wholly left out in the
frame), 2,359 kept a triangle the frame left out. `KF4_ND_PREP=0` and `KF4_SA_MEMO=0` turn each off;
`KF4_ND_PREP_CHECK=1` and `KF4_SA_MEMO_CHECK=1` take them and work them out too, and count what differs: 0 at the four
spots.

**Numbers.** `--fast`, 240, the owner's settings, 600 vblanks (760 under water), two runs each, d715daed -> rc/own; the
GS's thread a game frame (GIF time / frames handed over) and a recording:

| spot | GS thread a game frame | a recording |
|---|---|---|
| pond | 19.1 -> 17.7 ms | 8.72 -> 7.06 ms |
| fortress | 15.4 -> 14.4 ms | 8.21 -> 6.83 ms |
| canyon | 14.8 -> 14.3 ms | 6.36 -> 5.63 ms |
| under water | 11.7 -> 11.6 ms | 5.27 -> 5.14 ms |

Real time (`KF4_PRESENT_SIM=240`, 1,400 vblanks, 760 under water; one game at a time, the GPU otherwise idle, two runs
each; pond's first base run an outlier with 517 recordings):

| spot | pictures a second | GS thread a game frame | a recording | GPU a picture |
|---|---|---|---|---|
| pond | 91.7, 97.9 -> 97.8, 97.9 | 16.7-17.8 -> 16.5 ms | 9.3 -> 7.5 ms | 8.7 ms |
| fortress | 158.8, 158.2 -> 158.8, 159.1 | 14.1 -> 13.0 ms | 8.4-8.5 -> 6.9 ms | 5.5 ms |
| canyon | 161.1, 161.1 -> 160.8, 161.4 | 13.5 -> 12.8 ms | 6.3 -> 5.5 ms | 5.4 ms |
| under water | 202.3, 202.6 -> 202.6, 202.6 | 10.0 -> 9.9 ms | 5.3 -> 5.2 ms | 2.0 ms |

With the GPU to itself, the pond, the fortress and the canyon are now bound by the GPU, not the GS's thread: 8.7, 5.5
and 5.4 ms of GPU a picture shown (about 85% of the GPU's time at the pictures a second they reach), so the pictures a
second do not move; the GS's thread has room (half a game frame at the pond). The GS's time saved shows when the GPU
is shared (the owner's other game) or on a slower CPU. What would move the pictures a second at these spots is the
GPU's work a picture (each picture draws the whole scene: the zombies' shader-blended runs, the MSAA resolves).

**What the recording still costs** (the pond, sampled in `interp_record`): Granite's calls and the driver about 40%
(the pipeline binds: rc/dynstate's), the uploads asked for again about 15% (each written to VRAM again and its blocks
given back to it in the table), the native draws' walk about 19% (the register writes and CLUT loads, the fetch, the
moves, the corners copied), the scene batches' textures and the t 0 tests about 15%. The full (b) would move the
scene's calls into the frame but not the walk of the rest; it would also need the camera at t 0 in the frame
(frame_interp.c's `move_between` from the marks the GS sees) and the picture's scene state checked at each batch.

**Pictures:** every picture the same as d715daed's: the four spots (`--fast`, 240, the owner's settings, vblanks 200-207,
301, 399; under water 400-407, 600: 156 pictures, 0 differ); hosted (`--renderer native --native-draw on`) at the pond and
the fortress (80 pictures, 0 differ); the herb run in modern at the owner's settings and 240 (the same `--log`, PATH1
hash 561c91020b2908ee, 12 pictures). The off check passes: boot_test; frame_test `--renderer gs` and `native`;
gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0 of 90; herb with `--native-draw off` against play-932801f: the
same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles 460424273, pictures 1800, 1820 and 1840 byte for byte.

### Pipeline binds: dynamic state (branch `rc/dynstate`, from `native/draw` d715daed)

The replay's cost from pf/record: a pipeline bound for almost every draw, as the tie rule's Z-first and colour draws
(and pf/skin's marks and shines) differ in depth write, compare, colour mask, blending and stencil, which Granite
bakes into the pipeline.

**What changed** (`cmake/granite_dynamic_state.cmake` and `.inc`; RENDERER.md, "And with dynamic state"):
- **Granite with Vulkan's dynamic state**, for the native GS's command buffers only (two state bits `Cmd::raw` sets):
  depth test, write and compare, the depth bounds test, stencil test and ops, culling, front face, topology within
  its class and blend constants (Vulkan 1.3), and blend enable, equation and the colour write mask where the device
  has VK_EXT_extended_dynamic_state3 (the RTX 5080 does; lavapipe lists it too, not tried here). A pipeline is looked
  up without that state; before each draw Granite sets what differs from what it set last, as it would have baked it.
  The recording needs nothing new: its calls set Granite's state as before, and the replay's draws set Vulkan's.
- **The same pipeline not bound again** (after a render pass begins Granite bound whatever pipeline it found).
- `--stats`: "native GS pipelines bound: N for M draws; pictures between: B a recording, B a replay (for D draws)".
  `KF4_GSN_DYNAMIC=0` is the old pipelines (to compare), `1` without the blending (no extended dynamic state 3).

**What is left is programs.** Of the binds left (the pond, sampled), 98% change the program: the map's (sa_000),
the skinned models' (sa_280), the effects' (sa_700) and the GS's own alternate batch by batch. Joining them is
the shaders' work, not state's.

**Binds** (`--stats`, the four spots; `--fast` 600 vblanks unless said; before = `KF4_GSN_DYNAMIC=0`):

| spot | all binds / draws | a recording | a replay (real time, 240) |
|---|---|---|---|
| pond | 1,617,104 -> 411,637 / 1,432,552 | 3,567 -> 953 | 4,190 of 4,199 (fast) -> 890 of 3,411 |
| fortress | 2,106,325 -> 120,507 / 2,059,958 | 4,508 -> 256 | 4,652 of 4,668 (fast) -> 257 of 4,600 |
| canyon | 1,106,277 -> 19,187 / 1,106,784 | 1,558 -> 36 | 1,612 of 1,615 (fast) -> 36 of 1,594 |
| under water | 74,354 -> 12,635 / 74,217 | 158 -> 30 | 152 of 157 (fast) -> 31 of 162 |

**Time.** The GS's thread a game frame (`--fast`, 240, the owner's settings, 600 vblanks, two runs each, base
d715daed -> new; the owner was playing another game on the GPU): pond 24.2-24.4 -> 22.8-23.6 ms (a recording
11.0-11.1 -> 10.6-11.0), fortress 18.2-21.0 -> 16.8-18.0 (9.7-11.2 -> 9.2-9.9), canyon 19.4-23.6 -> 18.2-20.2
(8.2-9.5 -> 8.0-8.4); under water within its noise (13.6-23.4 against 12.3-27.4: the loading). A frame and its
recording bind far less but still work out each batch's state; the replay is where the binds were most of the
time. Real time (`KF4_PRESENT_SIM=240`, 1,400 vblanks, 760 under water, the GPU otherwise idle, two runs each,
alternating):

| spot | a replay | pictures a second |
|---|---|---|
| pond | 1.69-1.83 -> 1.59-1.61 ms | 83, 95 -> 97, 98 |
| fortress | 1.62-1.66 -> 0.96-0.98 ms | 150, 95 -> 126, 163 |
| canyon | 0.62-0.64 -> 0.37-0.38 ms | 66, 157 -> 166, 155 |
| under water | 0.18-0.19 -> 0.16-0.18 ms | 201, 105 -> 201, 192 |

The pictures a second move with the fallback's cuts and whatever else ran on the PC (a run's start found the GPU
at 98-99% twice); the replays' times are the steady figure.

**A first run compiles.** The pipelines are new to the driver's cache: the first run at the pond compiled six or so
of them at about 4 s each (the scene's large fragment shaders; the old ones took as long once, 2 s each in older
logs), then none again. Fewer pipelines in all (one for what were several), so less to compile later.

**Pictures:** every picture the same as d715daed's, game frames and pictures between (the four spots, `--fast`,
vblanks 200-207, 301, 399; under water 400-407, 600: 156 pictures, 27-30 of each spot's from the recording), also with
`KF4_GSN_DYNAMIC=1`; the pond hosted (`--renderer native --native-draw on`, 40 pictures); the herb in modern at the
owner's settings at 240 (the same `--log`, PATH1 hash 561c91020b2908ee, pictures 1800, 1820, 1840). The off check
passes: boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0
of 90; herb with `--native-draw off` against play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles
460424273, pictures 1800, 1820 and 1840 byte for byte.

### rc/own and rc/dynstate merged (`native/draw`, both on d715daed)

They merged without a conflict in the code (only this file's two sections). rc/own's hooks in `gsn.cpp`
(`forget_uploads`, the stats) and rc/dynstate's (`Cmd::raw`, the bind counters around `interp_record` and the replay)
are in different places; `gs.c` has both the new bind line and the stats printed hosted too. No shader changed.
Configure again after pulling: the Granite patch is applied at configure time.

**Pictures:** every picture the same as d715daed's: the four spots (`--fast`, 240, the owner's settings, vblanks
200-207, 301, 399; under water 400-407, 600: 156 pictures, 0 differ); hosted at the pond (`--renderer native
--native-draw on`, 40 pictures, 0 differ); the herb in modern at the owner's settings at 240 (the same `--log`, PATH1
hash 561c91020b2908ee, pictures 1800, 1820, 1840). The off check passes: boot_test; frame_test `--renderer gs` and
`native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0 of 90; herb with `--native-draw off` against
play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles 460424273, pictures 1800, 1820 and 1840 byte
for byte. In the merged build both are at work: at the pond 62,370 draws' records and 462,285 batches taken from the
frame, and 194,073 pipelines bound for 769,256 draws.

**The GS's thread a game frame** (`--fast`, 240, the owner's settings, 600 vblanks, 760 under water, two runs each
alternating, the GPU otherwise idle, d715daed -> merged; the frame's own is the GIF time less the pictures between):

| spot | in all | the frame's own | a recording | a replay |
|---|---|---|---|---|
| pond | 18.8 -> 16.4 ms | 10.2 -> 9.9 ms | 8.5-8.6 -> 6.5-6.6 ms | 2.5 -> 2.2-2.3 ms |
| fortress | 14.7-15.1 -> 12.1 ms | 6.8-7.0 -> 6.3 ms | 7.9-8.1 -> 5.8 ms | 1.8 -> 1.1-1.2 ms |
| canyon | 14.2-14.4 -> 12.4-12.5 ms | 8.1-8.2 -> 7.4-7.5 ms | 6.2 -> 5.0 ms | 0.8-0.85 -> 0.5-0.55 ms |
| under water | 10.6 -> 10.3-10.4 ms | 5.9-6.0 -> 5.8 ms | 4.6-4.7 -> 4.5 ms | 0.34 -> 0.30-0.33 ms |

**Real time** (`KF4_PRESENT_SIM=240`, 1,400 vblanks, 760 under water; one game at a time, the GPU otherwise idle, two
runs each alternating; the two runs agree within 0.3 a second):

| spot | pictures a second | cut to fewer (vblanks with fewer, with none) | a recording | a replay | GPU a picture |
|---|---|---|---|---|---|
| pond | 96.6-96.7 -> 100.5-100.6 | 8 (1,207, 0) -> 7 (1,177, 0) | 9.3 -> 7.1 ms | 1.82 -> 1.53 ms | 8.64 -> 8.36 ms |
| fortress | 155.8-156.0 -> 168.6-168.7 | 1 (30, 0) -> 1 (30, 0) | 8.4 -> 6.0-6.1 ms | 1.59 -> 0.95 ms | 5.44 -> 5.08 ms |
| canyon | 158.2-158.5 -> 169.4-169.7 | 1 (30, 0) -> 1 (30, 0) | 6.3 -> 5.1 ms | 0.61 -> 0.36 ms | 5.35 -> 5.04 ms |
| under water | 200.9 -> 204.0 | 1 (30, 0) -> 0 | 5.2-5.3 -> 5.1 ms | 0.17 -> 0.16 ms | 2.04 -> 2.01 ms |

With the binds gone the GPU's time a picture falls too (about 6% at the fortress and the canyon), which is what moves
the pictures a second there: these spots are bound by the GPU. The pond is still bound by it (8.4 ms a picture).
What is left: the GPU's work a picture, the programs' binds (rc/dynstate), and the recording's walk of the packets
(rc/own's open item).

### GPU cost at the owner's spots (branch `p8/gpu-cost`, from `native/draw` e1f8da20)

After rc/own and rc/dynstate the pond, the fortress and the canyon were bound by the GPU: 8.4, 5.1 and 5.0 ms a
picture where 240 needs about 4.1.

**Where the time went** (`KF4_GSN_GPU_TIME=2`, new: a timestamp at each change of what a render pass draws, kept in
the recording so that the replays are split too; ms of GPU a picture, e1f8da20, headless 240, the owner's settings;
the marks wait for the GPU's work before them, so read the figures as shares):

| | pond | fortress | canyon | under water |
|---|---|---|---|---|
| in all | 8.49 | 5.20 | 5.14 | 2.02 |
| shader blending's in-pass copies, pictures between (scene + reflection) | 3.18 + 0.46 | 0.19 | 0.02 | 0 |
| ... in the game's frames | 0.06 + 0.03 | 0.03 | 0 | 0 |
| shader blending's runs (scene + reflection) | 0.24 + 0.12 | 0.19 | 0 | 0 |
| the scene's draws (colour) | 1.48 | 1.80 | 3.01 | 0.54 |
| the ties' Z-first draws (all alpha-tested) | 0.95 | 1.99 | 0.71 | 0.27 |
| the reflection's draws + Z-first | 0.42 + 0.41 | | | |
| skinned groups (first passes, marks, shines; scene + reflection) | 0.18 + 0.06 | 0.25 | 0.30 | 0 |
| render passes begun, and draws not tagged | 0.24 | 0.29 | 0.42 | 0.40 |
| the turns' MSAA loads and depth | 0.13 | 0.09 | 0.10 | 0.14 |
| target copies, resolves, pass ends, other | 0.18 + 0.12 + 0.01 + 0.06 | 0.14 + 0.03 + 0.01 + 0.04 | 0.20 + 0.03 + 0.03 + 0.02 | 0.29 + 0.04 + 0.07 + 0.02 |
| the GS's own batches, the scene's ops (clears, alpha) | 0.05, 0.09 | 0.10, 0.04 | 0.20, 0.06 | 0.07, 0.17 |
| cut-outs shaded at every sample | 0 | 0.01 (63 draws) | 0.02 (103 draws) | 0 |
| the shadows (stencil) | 0.01 | 0.002 | 0.005 | 0 |
| the scanout | 0.02 | 0.02 | 0.02 | 0.02 |

Not costs: MSAA resolves (0.005-0.03 ms a picture: the scanout's and the turns'; shader blending resolves in the pass);
the composite (no picture composited: the scene is at the GS's scale here); the shadows' stencil; the cut-outs' shading
at every sample. Windowed at the owner's settings (fullscreen 2560 x 1440, best scaling) the scanout with the
presenter's scaling is 0.17 ms a picture, and the flash recorder costs nothing measurable (4,313 pictures with it,
4,294 without, the same run). **A recording does not draw the frame again**: it is the frame's first picture between
(`interp_record` draws that picture and keeps its calls); every picture the GPU draws is shown. What made each picture
between cost more than the frame was the copies below.

**What changed** (pictures byte for byte e1f8da20's):
- **The copy a run blends from, by the run's own triangles** (`gsn_standalone.inc`, `sa_copy_run.frag`). Shader
  blending copies the scene's colour into the pass's scratch before each run, over the run's box. In a picture between
  that box is the whole scene for a skinned model's run (the GPU moves its corners: `nd_box_at_0` cannot say where), so
  each run of the pond's blended zombies (ALPHA `0x44` with As up to 0x88, in the scene and in the reflection) copied
  and resolved the whole 3x MSAA scene: about 230 a picture, 1.1 GB. The copy is now drawn with the run's own vertex
  shader and triangles, rasterized conservatively (overestimated: every pixel a triangle touches, all its samples), the
  target's pixels as `pass_copy.frag` / `pass_resolve.frag` make them. The run's draws read the copy only at pixels its
  triangles cover, all inside it. `KF4_SA_RUN_COPY=0`: the box (to compare). Pond 8.49 -> 5.7 ms.
- **The CLUT a storage buffer** (`gs_frag.h`'s `Clut`, every program of the native GS: the scene's, the PS2 path's,
  hosted). A texel's CLUT entry is read at an index of its own; a uniform buffer read at indices that differ across the
  pixels shaded together is read once for each, one after another. As a storage buffer the same reads go through the
  cache: fortress 5.04 -> 3.73 ms, canyon 5.08 -> 3.73, pond 5.7 -> 4.27. A CLUT the same as one of the last 64 in
  the constant chunk is bound where that one is (`Cmd::set_storage_data`: Granite makes a descriptor set for each
  storage buffer place; the same place, a set it has). The replays bind what the recording bound (`OP_STORAGE_DATA`).
- **`KF4_SA_REFLECTION_KEEP`** (off by default here: it could change pixels; exact and on by default since `p8/pond240`, below). 1: while the picture's camera is the frame's
  (its move within 1e-6: a camera that did not move comes with that much rounding), a picture between leaves the
  reflection as the frame drew it: its clear and every batch into it left out (`sat_kept`, from `sa_flush`). The
  reflection's draws move with the camera only (`nd_model_flags`), so with no move they would land on the frame's
  pixels; the rounding's move leaves X and Y as the frame's but can move an unrounded depth or S, T, Q by a last bit,
  which can turn a tie, hence the switch. A recording made with the other choice is made again (reason 6). 2: always
  (the reflection then moves 30 times a second). At the pond standing still, 1 leaves out about 70,000 batches in 400
  vblanks and no pixel of 40 pictures differs; turning (`turn.pad`), 1 keeps only the still pictures (none differ),
  2 differs in up to 51,000 pixels a picture (worst 25 levels) on the water.
- `KF4_GSN_GPU_TIME=2` (above), and the draws and triangles a picture by the same kinds.

**What is left** (the pond at 1x without MSAA still takes 2.75 ms: most of a picture's GPU time does not scale with
the pixels). About 3,800 Vulkan draws a picture at the pond and 4,300 at the fortress. The ties' alpha-tested Z-first
draws are the costly ones: left out (a test, wrong pictures) the pond at 1x takes 0.6 ms less; drawn twice, 0.09 more;
their texture is not it (read without it, 0.06 less). Toggling the colour mask, depth write, compare or push constants
between two draws costs 0.03-0.14 ms in all. So it is the colour draw after its Z-first draw that waits: an
alpha-tested draw that writes Z tests late (after the shader's discard), and the next draw's early test over the same
pixels waits for it. Drawing the colour first and the Z after (exact for a run of triangles that do not overlap) was
slower (pond 4.25 -> 4.62 ms). Left: an alpha-test-free Z pass where the texture cannot fail it (T8's try: few such
at the far save), or fewer, larger batches.

**Numbers** (real time, `KF4_PRESENT_SIM=240`, headless, 1,400 vblanks, 760 under water; one game at a time, the GPU
otherwise idle, base and new alternating, two runs each, the pipeline cache warm for both; pictures handed over a second
of wall time, loading included, so about 215 is all of 240):

| spot | pictures a second | GPU a picture | replays (in all) |
|---|---|---|---|
| pond | 101.2, 101.2 -> 192.5, 192.9 | 8.33 -> 4.26 ms | 1,031 -> 3,230 (7 cuts -> none) |
| pond, `KF4_SA_REFLECTION_KEEP=1` | -> 217.0, 217.0 | -> 2.93 ms | |
| fortress | 168.7, 169.0 -> 217.0, 217.2 | 5.07 -> 3.70 ms | 2,655 -> 3,813 |
| canyon | 170.8, 170.7 -> 217.2, 226.6 | 5.03 -> 3.69 ms | 2,700 -> 3,815 |
| under water | 204.2, 204.2 -> 204.2, 204.0 | 2.01 -> 1.51 ms | 1,898 -> 1,897 |

Windowed (the owner's settings-4: fullscreen 2560 x 1440, best scaling, flash recorder on, 1,400 vblanks):

| spot | pictures a second | GPU a picture |
|---|---|---|
| pond | 94, 101 -> 183, 177 | 7.9-8.3 -> 4.40-4.42 ms |
| pond, `KF4_SA_REFLECTION_KEEP=1` | -> 217 | -> 3.11 ms |
| fortress | 174, 200 -> 213, 202 | 4.27-4.71 -> 3.98-4.00 ms |
| canyon | 183, 184 -> 217, 208 | 4.68-4.69 -> 3.51-3.52 ms |

The fortress and the canyon now keep up at 240 (the game's 30 frames and every picture between); the pond standing
still with `KF4_SA_REFLECTION_KEEP=1` too. The pond by default is still bound by the GPU (4.3-4.4 ms a picture: the
draws above).

**Pictures:** byte for byte e1f8da20's: the four spots (`--fast`, 240, the owner's settings, vblanks 200-207, 301, 399;
under water 400-407, 600: 156 pictures, 0 differ, with `KF4_SA_RUN_COPY` and the CLUT both on), the pond turning
(`turn.pad`, 40 pictures), hosted at the pond (`--renderer native --native-draw on`, 40 pictures), the herb run in
modern at the owner's settings at 240 (the same `--log`, PATH1 hash 561c91020b2908ee, pictures 1800, 1820, 1840). The off check passes:
boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0 of 90; herb with
`--native-draw off` against play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles 460424273, pictures
1800, 1820 and 1840 byte for byte.

### The pond at 240 (branch `p8/pond240`, from `native/draw` 96e22814)

**The reflection kept, exact, by default.** `KF4_SA_REFLECTION_KEEP` is 1 by default, and 1 now means the camera of the
frame shown is the frame's *bit for bit*: `frame_interp.c` compares the two frames' view fields (the rotation and the
translation the game wrote) and hands the answer over with the frame (`GsnInterpFrame`'s `still`), so it holds for all of
the frame's pictures between and a recording never disagrees with its replays. Every camera between two equal cameras is
that camera, and the reflection's draws move with the camera only (`nd_model_flags`; no run of the reflection is paired
with its model), so the frame's reflection *is* each picture's: no tolerance to prove. The old test (the move's floats
within 1e-6) was of the camera `camera_between` rebuilds through a quaternion, a rounding off the frame's; drawn from it,
the reflection could differ in a last bit of depth. Seen once: a frame after looking around (the right stick) and
stopping, 7 pictures between in which 96e22814 differs from its own frame in 11 pixels (worst 5 levels) of the water; kept,
they are the frame's to the bit. 0: drawn in every picture between; 2: always kept (moves at 30). A camera that moves
draws the reflection in every picture, as before.

**The Z-first draws by depth alone** (`sa_zfirst.frag`, `KF4_SA_ZFIRST=0` to compare). A tie's Z-first draw (no colour, Z
written) of a pass no pixel can fail (no alpha test, or the batch's alpha range, the table's texture alpha with the
corners' As, inside what the test passes; no DATE, no SCANMSK) is drawn with an empty fragment shader that declares early
tests: the same vertex shader, so the same pixels, depth and stencil, nothing to discard. Exact, but it hardly applies:
at the pond about 1 in 40 of the alpha-tested Z-first draws qualify (2,708 draws in 400 vblanks); the others are layers
whose corners fade to As 0 under ATST GREATER 0 (the texture's alpha is 0x80 or not read at all), so pixels near those
corners do fail the test and their Z must not be written. Pond 2.96 -> 2.94 ms a picture. An upper bound, every alpha-tested
Z-first draw drawn so (wrong pictures): 3.12 -> 2.85 ms (`KF4_GSN_GPU_TIME=2`), "Z-first atest" 0.78 -> 0.11 and the
draws after them 0.96 -> 0.84: about 0.27 ms at stake. Taking it exactly needs the draw split by triangle (the triangles
whose three corners pass, by depth alone; the rest with the test; the Z-first draw's order does not matter, as it keeps
the nearest), which needs each corner's As where the batch is planned (the vertex shaders light the corners): left.
- **Tried, not kept:** `layout(early_fragment_tests)` on the colour draws that write no depth or stencil (a `gs.frag`
  variant): slower, 2.94 -> 3.01-3.03 ms at the pond (the driver already tests those early; forcing it only lost what it
  does around the discard).

**Numbers** (real time, `KF4_PRESENT_SIM=240`, headless, 1,400 vblanks, 760 under water; 96e22814 and new alternating, two
runs each, the pipeline cache warm; about 215 a second is all of 240):

| spot | pictures a second | GPU a picture |
|---|---|---|
| pond | 192.4, 193.3 -> 217.0, 217.0 | 4.25 -> 2.93 ms |
| pond turning (left held from vblank 200) | 200.4, 209.5 -> 201.0, 209.5 | 2.62 -> 2.58 ms |
| fortress | 217.0, 217.0 -> 215.8 (a cut), 217.0 | 3.69 -> 3.69 ms |
| canyon | 217.0, 217.0 -> 217.2, 217.2 | 3.69 -> 3.69 ms |
| under water | 204.2, 204.2 -> 204.2, 204.2 | 1.51 -> 1.51 ms |

Windowed (the owner's settings-4: fullscreen 2560 x 1440, best scaling, flash recorder on, 1,400 vblanks):

| spot | pictures a second | GPU a picture |
|---|---|---|
| pond | 185.8, 179.0 -> 208.4, 217.0 | 4.43 -> 3.09 ms |
| fortress | 216.4, 213.3 -> 209.2, 214.6 (cuts both) | 3.94-3.97 -> 3.93-4.02 ms |
| canyon | 208.5, 217.2 -> 217.2, 208.5 | 3.51 -> 3.51 ms |

The pond now keeps up at 240 by default where the owner stands; turning (the reflection drawn in every picture) it is
not bound by the GPU (2.6 ms a picture).

**Pictures:** byte for byte 96e22814's: the four spots (`--fast`, 240, the owner's settings, vblanks 200-207, 301, 399;
under water 400-407, 600: 156 pictures), the pond turning, walking, walking and turning, and the d-pad's right (each held
from vblank 195; every picture of vblanks 200-207 and 220-223: 192), and turning or walking for 12 vblanks then stopping
(every picture of vblanks 200-231: 256, the stops' pictures kept); hosted at the pond (`--native-draw on`, 40 pictures);
the herb run in modern at the owner's settings (the same `--log`, PATH1 hash 561c91020b2908ee, pictures 1800, 1820 and
1840). The off check passes: boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7,
1234); vu1_test 0 of 90; herb with `--native-draw off` against play-932801f: the same `--log`, PATH1 hash
204e443a9fc49731, VU1 cycles 460424273, pictures 1800, 1820 and 1840 byte for byte.

### Death in the deadly water (branch `p8/death-water`, from `native/draw` 341eb444)

The owner, `--renderer modern` at 240: falling into a deadly water spot (`bugs_modern/quick_deathwater`, walking
forward) showed garbage tiles, noise and light-cyan bands over the screen for a few seconds, the HUD intact. Not native
draw's, nor the perf work's: the same at 240 on the PS2 path (`--native-draw off`), hosted, 5111e87, 73887b9 and
play-932801f; never at 30. Headless with `--fast` it is every picture between from the death screen's first frames to
the fade's end (vblanks 446-468 from the save with `UP` held from 250); real time, whenever the death screen's camera
shows the texture below.

**The cause** (`frame_interp.c`, `flat_copies`): the pictures between make the wobble's copy of the frame (to 0x2BC0)
again only when the frame does not read those pages before the copy without writing them first (a read of the copy of
the frame before). The death screen draws a 256 x 256 PSMT8 texture at 0x37E0 with one mipmap (TEX1's MTBA: the level
packed after it), early in the frame. A texture with mipmaps was taken as read over twice its first level: 16 pages,
447-462, where it is 10 (447-456, inside the frame's upload 426-457). Pages 458-462 had not been written yet in the
frame, so the copy was "read before", not made again, and the pictures between drew the wobble's strips from the
textures uploaded over 0x2BC0 after the frame's copy. A texture with MTBA and up to 3 levels is now read over its
levels exactly (as `gsn.cpp`'s `mip_addresses` packs them); others keep twice the first level.

**Checked:** the death at 240 (3x, the owner's settings; `--fast` three walk timings, every picture of vblanks
350-480; real time `KF4_PRESENT_SIM=240` twice; 1x; hosted; the PS2 path): no picture between differs from its frame by
more than the fall's own motion; at 30 the same. Byte for byte 341eb444's: the four spots (156 pictures), hosted at the
pond (40), the herb run in modern at the owner's settings (the same `--log`, PATH1 hash 561c91020b2908ee). The off
check passes: boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0
of 90; herb with `--native-draw off` against play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles
460424273, pictures 1800, 1820 and 1840 byte for byte.

### The first visit's freeze: pipelines made ahead (branch `p8/warmup`, from `native/draw` 341eb444)

The owner (341eb444, `--renderer modern` at 240, just rebooted): the first time he walks into an area the screen stands
still for 1-2 seconds; afterwards that area is fine. **What it was:** Vulkan pipelines made when a draw first needed
them, each a second or more. Measured with `KF4_HITCH_MS` (RENDERER.md, "Finding hitches"; headless, `KF4_PRESENT_SIM=240`,
the owner's settings, a fresh data folder): walking in the canyon (map 99) the GS's thread stood 3.3 s at the boot
picture, 13.6 s and 4.6 s at the first frames after the load (8 and 4 pipelines; one pipeline alone 2.8 s), and 0.85 s
in the middle of the walk (two pipelines of the pictures between, first needed there). The game's thread waited on it
the same. Nothing else came near: the meshes of a newly loaded map put on the GPU (732-1,226 at once), texture decodes
and the disc reads are 20-45 ms together, at the load's first frame only; walking, no vblank's work went over about 22 ms
(the steady 18-22 ms of a vblank at 240) once the pipelines were made. So no mesh work moved to `map_load`.
- **Why seconds, and why again:** NVIDIA's compiler takes 2-4 s for the GS's big fragment shader with a state it has not
  seen. Its disk cache (`%LOCALAPPDATA%\NVIDIA\GLCache`) is kept over a reboot, but per program (a renamed kf4.exe starts
  empty) and this PC's was at its 1 GB limit (1,017 MB), evicting; each new build changes some shaders too. Ours
  (`native_gs_pipelines.bin`) was written only every ten seconds when it had grown, never at exit: a crash or a quit soon
  after an area's first visit lost them.
- **The fix** (RENDERER.md, "Pipelines made ahead"): every pipeline written down, a list of them shipped in the program
  (59 at the owner's settings from a tour of the 64 maps turning, the pond, the canyon walk and the menus, and the default
  settings' tours), the data folder's list for the rest; at the start they are made on threads of their own, those the
  cache has at once, the rest with "Building shaders... N/M" in the window's corner; play waits for them where it starts.
  The cache and the list are written two seconds after pipelines are made, and at exit.

**Numbers** (the canyon walk, 1,500 vblanks, the owner's settings, headless real time; spikes over 20 ms of the GS's
thread):

| | before | after |
|---|---|---|
| a new driver cache (a renamed kf4.exe) and a fresh data folder | 3.3 s (boot), 13.6 s and 4.6 s (the load), 0.85 s walking | 59 pipelines built in 13.2 s behind the title (8 threads); play waited 7.0 s with the note; 4.1 s at the boot picture (its first pipeline, needed at once); after: none walking, 26 ms at the load's first frame |
| the same again (warm) | none walking; 21-32 ms at the load's first frame | 59 ready from the cache at once; the same |

The pond (39 pipelines of its own): before, a fresh data folder 2.8 s at the boot picture, 6.4 s and 2.9 s at the load; with the list and the
cache, nothing over 40 ms (the load's first frame, its 1,226 meshes put on the GPU, 32-42 ms). The data folder's list catches
what the shipped one lacks (the log says so; `--stats`: "pipelines made ahead").

**Pictures:** byte for byte 341eb444's at the four spots (`--fast`, 240, the owner's settings, every picture of vblanks
200-207, 301, 399; under water 400-407, 600: 156 pictures), the herb run in modern at the owner's settings (the same
`--log`, PATH1 hash 561c91020b2908ee, pictures 1800, 1820, 1840). The off check passes: boot_test; frame_test `--renderer
gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0 of 90; herb with `--native-draw off` against
play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles 460424273, pictures byte for byte.

### The Queen's Palace: the MP drain at 240 (branch `p8/queen`, from `native/draw` 6234768a)

The owner, `--renderer modern` at 240 (settings-4): teleported to the Queen's Palace (map 022, its start) and walking a few
feet forward, the picture turns purple and wobbles and the F3 counter reads `27 FPS / 14 GAME CUT 2`: the game's own frames
at 14 a second. The effect is the MP drain (`GameLoop_DrawFrame`: `mp_drain_tint`, colour 0x46 0x32 0x8C at full drain):
`GsDraw_CopyFrame(0x2BC0)` and `Game_DrawWobblingScreen`, the under-water wobble tinted, then the effects' textures uploaded
over the copy (`Effect_UploadTextures`, "if tinted"). The spot: `bugs_modern/quick_queen_start` (the teleport's save) with
`UP` held from vblank 200 for 150 (the drain starts at vblank 290), or `bugs_modern/quick_queen` (taken at 400, the drain on).

**What it was** (`KF4_HITCH_MS`, a frame's vblank on the GS's thread 80-110 ms; `--stats`, a decode of 0x2BC0 as a 1024 x 512
texture each game frame, 55-65 ms, and a write-back or two):
- **The restore drew the whole frame again, every game frame.** While pictures between are drawn, a buffer the next frame reads
  before it draws it (here the flames' two 128 x 128 scrolling textures, 0x3840 and 0x3A40) is drawn again as the frame left it
  after the vblank's pictures (`port_interp_end_vblank`: the frame walked once more, "exact"). The wobble keeps the pictures
  between moving, so that was every game frame here (walking without the drain, only while walking). Its draws into the frame
  were meant to be left out (they go into the buffer shown, which the next frame clears: no vblank shows them): the 3D's
  vertices got "ADC", but the bit set was the qword's 127th, not ADC's 111th, so nothing was left out; and native draw's
  world is not VU1's vertices at all. So each restore drew the world again (about 15 ms on the GS's thread), and its wobble,
  whose copy of the frame a restore does not make (only copies the pictures between read after them are made again), read
  0x2BC0 from VRAM: the scene's copy let go by the uploads over it, the GS's target there 448 rows, the texture read as
  1024 x 512 (the scene's `read_extent` takes U and V slightly below 0 as reading everything), decoded at 1x after the frame
  and Z buffer went back to VRAM. Now `exact_kick` sets ADC (bit 111) on every vertex kick into the frame, 3D and 2D, A+D
  kicks become XYZ3, and a native draw into the frame is left out (`exact_frame`); `KF4_INTERP_RESTORE_FRAME=1` draws as
  before. The PS2 path and hosted gain the same.
- **A write-back a frame for half a page.** The copy at 0x2BC0 becomes a shadow when the effects' textures go up over it; a
  64 x 64 PSMT8 one at 0x3780 fills half a page, and its draw decoded it from VRAM each frame after claiming its page: the
  page went back (a read-back waiting for the GPU, 3-4 ms at 3x). A texture decode now leaves a shadow's page owed when every
  bit it reads was written since the shadow was made (`claim_texture`, `shadow_covers`; `KF4_GSN_SHADOW_READS=0` to compare).
- **Each upload over the shadow swizzled again.** `shadow_written` notes an upload's bits in the mask by swizzling a buffer of
  ones; with the shadow left over the copy, every upload of every picture between did so (27 us each, 67 a picture). An upload
  of whole pages now sets theirs whole, and only the pages a transfer writes are looked at.

**Numbers** (the GS's thread, `--fast` at 240 so that every picture is drawn, the owner's settings, the palace walk: 900
vblanks from `quick_queen_start`, the drain from 290; the owner was playing on the same PC, so real time was noisy):

| | 6234768a | new |
|---|---|---|
| GIF time (draws, write-backs, decodes) | 34.4-38.8 s (24-29, 3-5.6, 17-19) | 7.1-7.6 s (2.4-2.6, 0, 0.05) |
| a picture between drawn from the packets (the recording) | 14-17 ms | 7.5-8.2 ms (5.7-7.6 without the drain) |
| wall | 40-48 s | 12.5-17.8 s |
| without the drain (`walk.pad`) | 5.7 s, 10.2 s wall | 5.4 s, 9.9 s wall |

The same 601 vblanks by renderer: modern 19.2 -> 4.8 s of GIF, hosted (`--renderer native --native-draw on`) 25.0 -> 10.7 s,
the PS2 path (`--native-draw off`) 19.3 -> 5.0 s. native-5111e87: 100.7 s (900 vblanks). At 30 it did not drop (a frame 27 ms
of the GS's thread with its write-back, 29.4 game frames a second). Real time headless (`KF4_PRESENT_SIM=240`, 1,100 vblanks,
with the owner's game on the GPU): 6234768a 1,159-1,540 pictures handed over (the cut at none: 60 a second), new 1,745-2,943
(up to 240 a second in the last half second, the game at 30); without the drain 3,700-4,000. GPU 1.8-2.8 ms a picture alone
(`KF4_GSN_GPU_TIME=2`: not bound by the GPU). No pipeline is made in play here (a fresh data folder, 240 and 30: "0 not on a
list").

**What is left:** a picture between's own copy at 0x2BC0 (`replay_only`) takes each page an upload writes over it from VRAM at
once (`sync_targets`), converted on the CPU at the scale (about 1.4 s of the 7 s above): the pages could be taken at 1x and
scaled on the GPU, or only when the target is next read.

**Pictures:** byte for byte 6234768a's: the four spots (`--fast`, 240, the owner's settings, every picture of vblanks 200-207,
301, 399; under water 400-407, 600: 156 pictures), the palace (every picture of 280-300, 400-407 and 600: 120), hosted at the
pond (40) and at the palace (120), the PS2 path at the palace (120); the herb run in modern at the owner's settings
(the same `--log`, PATH1 hash 561c91020b2908ee, every picture of 1800, 1820 and 1840). The off check passes:
boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0 of 90; herb with
`--native-draw off` against play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles 460424273, pictures
1800, 1820 and 1840 byte for byte.

### The wand's passage: the map change's fade at 60-240 (branch `fix/wand-teleport`, from main fd3956b7)

The owner, `--renderer modern` at 240 (settings of 10-25): standing on a Symbol of Passage, the menu, Item, Up x4 to the
Green Wand, Cross: it teleports, but for about a second the screen is corrupt, then clean. The spot:
`bugs1025/quick_wand` with `title.pad` and CIRCLE 500, CROSS 560, UP 620/640/660/680, CROSS 720 (`screen_fx_test.py`'s
`wand`). The wand starts the map change (`MapChange_StartPassage`); `MapChange_RunToEnd` draws one frame and copies it
to 0x2BC0 (`MapChange_CaptureFrame`, `GsDraw_CopyFrame`), then its own loop draws that copy in ten strips
(`MapChange_FadeOutCapture`, vblanks 742-749 here), black, and the new map's fade in.

**Which renderers:** all of them at 60-240, none at 30. Headless `--fast`: at 240 and 60 the fade's eight frames are
garbage (the textures of the map, the HUD intact above them) in modern at the owner's settings, `--native-draw
on,passes`, `--native-draw off` and paraLLEl-GS (`--renderer gs`); at 30 every renderer draws the last frame. So
not the modern renderer nor native draw: the pictures between (`frame_interp.c`).

**The cause** (`flat_copies`): a copy of the frame is made again in the pictures between only when the frame reads
it after the copy (the wobble); the map change's is read by the frames after, so it was left "as the game's frame
made it". But a picture between sends the frame's uploads again (`PORT_GS_REG_REPLAY_UPLOAD`, or the data), and this
frame uploaded its textures through 0x2BC0-0x3D3F before the copy: after the vblank's pictures between, the copy's
pages held those textures, and the fade drew them. Nothing restores them: the exact restore
(`port_interp_end_vblank`) ran only for a buffer the frame reads before it draws it, and left every copy out.

**The fix:** such a copy is `COPY_RESTORE` (not read after, over pages the frame uploaded or drew into before it,
the frame not drawn into after it, so the frame at its end is what was copied). It sets `restore_copy`, so the exact
restore runs after the pictures between (whether or not the frame before had 2D to pair); in it the frame's uploads go
again in their order and then the copy, its BITBLTBUF source left at the game's frame (the restore draws nothing into
the frame, and a copy made in it is only a `COPY_RESTORE`). A copy read after stays `COPY_BETWEEN`, made from the
picture between's frame as before; one into a frame drawn after it (none in the game) stays as before. Cost: one exact
restore at the map change ("1 frames drawn again" in `--stats`).

**Checked** (before, the fade's frames at 240 showed rows of the map's textures over the screen; after, the room):
- The fade's frames at 240 are the 30 run's byte for byte, vblanks 740-750: modern at the owner's settings, `--native-draw
  off` (3x widescreen), paraLLEl-GS (1x); `--native-draw on,passes` at 240 and modern at 60 (3x) clean by eye; real time
  (`KF4_PRESENT_SIM=240`, modern, the owner's settings) clean. (Vblank 739, a picture between of the still camera,
  differs from 30's by 2 levels at 273 pixels, as before the fix.)
- `screen_fx_test.py --cases wand --pictures --spots bugs1025`: before 8 of the fade's 10 game frames differ from 30's,
  after 0 of 10; flash_find 0 of 18 flagged.
- Byte for byte fd3956b7's: the four spots (`--fast`, 240, the owner's settings, every picture of vblanks 200-207, 301,
  399; under water 400-407, 600: 156 pictures); the herb run in modern at the owner's settings (the same `--log`,
  PATH1 hash 561c91020b2908ee with `--frames 1841`, pictures 1800, 1820, 1840 with every phase: 12). The off check
  passes: boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0 of
  90; herb with `--native-draw off` against play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles
  460424273 (`--frames 1841`: with 1881, both builds give 197d355532a43933 and 482017306), pictures 1800, 1820 and 1840
  byte for byte.

### Uploads: hashed once, pages known unchanged (branch `p8/uploads`, from `main` fd3956b7)

The game sends about 6 MB of textures a game frame on map 000 (172 uploads in pace_test's window), and the pictures
between ask for each again (`replay_upload`). Sampled on 000 (the owner's settings, 170 Hz real time, 173a8ffe), the
uploads were 16.7% of the GS's thread's busy samples: `image_data` 8.7 (`sa_uploaded` 4.6, of it `sa_own` 1.9,
`upload_hash` 1.4 and the compare with the chunk 1.1; `write_page_image` 1.5; `upload_hash` again 1.3) and
`replay_upload` 8.0 (`flush_batch` 2.3, `write_page_image` 1.9, `sa_upload_replayed`'s `sa_own` 1.7, `sync_targets` 1.6).

**What changed:**
- **Hashed once.** `complete_upload` works out `upload_hash` first and hands it to `sa_uploaded`, which hashed the
  same bytes again (the hook gains an argument; the table's other callers, an upload read back from VRAM and a copy
  kept as an upload, hash through a wrapper as before). The table still compares the bytes with its chunk's.
- **Pages known unchanged.** `write_page_image` compared each 8 KB page of an upload found again with the CPU's
  VRAM. Each page now remembers the image it was last made the same as (`page_holders`: the image's serial, never
  reused; the page's place in it; `page_written` then), and while nothing has written the page since it is not
  compared again (`--stats`: "pages known unchanged"; RENDERER.md has the rule it relies on). Most found-again
  uploads do change their pages (on 000, 123,137 of 134,472 copy at least one: the textures take turns in the same
  VRAM), so it skips 150,934 pages a run on 000 and 474,088 on 009.

**Numbers** (pace_test `--mode cost`, three rounds alternating, pinned to the P-cores; the uploads' ms a game frame
are the hitch lines' "uploads", the native GS's upload timer, over pace_test's window; i9-13900KF, RTX 4090):

| | main | hashed once | both |
|---|---|---|---|
| 000: uploads ms a game frame | 2.51-2.61 | 2.27-2.42 | 2.27-2.38 |
| 000: GS thread ms a game frame | 17.5-18.1 | 17.1-18.2 | 17.3-18.5 |
| 009: uploads ms a game frame | 6.46-6.72 | 6.09-6.16 | 5.82-6.03 |
| 009: GS thread ms a game frame | 21.4-22.2 | 21.1-22.7 | 20.5-21.2 |

About 0.25 ms a game frame on 000 and 0.6 on 009, a tenth of the uploads' time; the thread's own spread between runs
is as large on 000. Of 009's 6 ms, 3.1 is `sync_targets` after the uploads: the pictures between's own copy at 0x2BC0
taking each page uploaded over it from VRAM at once ("What is left" in the palace's section above), the largest piece
left near the uploads. The table's ownership (`sa_own`, 3.6% on 000 with the uploads asked for again) is next.

**Pictures:** byte for byte main's (191 pictures, the same `--log` each): every picture of vblanks 2300-2306, 2400,
2401, 2450 and 2500 after the teleport to 000 and to 009 at 240 (`--fast`, `KF4_INTERP_DRAW_ALL=1`, 44 each), in modern
at the owner's settings and hosted (`--renderer native`, 009 at 4x, 000 at 2x); the herb run in modern at the owner's
settings at 240 (PATH1 hash 561c91020b2908ee, every picture of 1800, 1820, 1840). The four spots' quick saves are not
on this PC. The off check passes: boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300 (seeds
1, 7, 1234); gsn_vram_test; vu1_test 0 of 90; herb with `--native-draw off`: the same `--log`, PATH1 hash
204e443a9fc49731, VU1 cycles 460424273, pictures 1800, 1820 and 1840 byte for byte. Rebased on main 74ecff65 (#73, #75),
the 191 pictures again byte for byte that main's.

#### A lazy CPU VRAM: cost and risk (not built)

The idea: an upload found again would not write the CPU's VRAM. Its pages would be owed (the image and the page's
place in it), written only when something reads them, and a page owed twice before a read written once. It is mostly
for modern, where the scene's textures and CLUTs come from the table.

**What it could save** is at most `write_page_image`'s compares and copies: 3.3% of the thread on 000 before this
branch (1.5 the frame's uploads, 1.9 the pictures between's), about 0.6 ms a game frame, and still 3.6% after it
(sampled on main's build, `upload_hash` 2.7 -> 1.2%): on 000 few of the pages it copies are ones it would not
change. It saves neither the hash (it finds the image) nor the table's compare and ownership, and
not `sync_targets`, which reads the very pages it would owe (on 009, 3.1 ms a game frame). Hosted decodes its
textures from the CPU's VRAM and would write nearly every page it owed.

**What reads an owed page** and would have to write it first (a reader missed reads old texels, with no error):
- `claim_pages`, which covers the read-backs, a local copy's source, `gsn_read_rect` and `gsn_write_rect`, and the
  CLUT load at every TEX0;
- `claim_texture` where `shadow_covers` lets it skip `claim_pages` (it calls `claim_targets` only), then
  `texture_hash` and the decode;
- `load_target_rect`: a target made over the pages (`get_target_made`), `sync_targets` after an upload;
- `shadow_resolve`, which copies the pages aside and merges them back;
- a write of part of a page: an upload not found again or not of whole pages (`swizzle_upload`, `settle_upload`),
  a 24-bit image (`rgb_only` keeps the words' top bytes), `write_back`'s rows, `gsn_vram_copy`'s destination,
  `gsn_write_rect`;
- the table's reads (`sa_uploaded_from_vram`, the copy kept as an upload in `gsn_scene_targets.inc`, `KF4_SA_VERIFY`);
- `gsn_vram()` (captures, `--renderer both`), the save state, kf4-gsreplay.

**The counts.** `page_written` feeds `texture_current`, the CLUT load's memo and the pictures between's "VRAM
written since the recording". A page is stamped only when its bytes change; owed, whether they would change is
not known without the compare that is being saved, so owing a page would have to stamp it (only the pages not
already the image's, with `page_holders`). Each such stamp is a miss for the CLUT memo and for `texture_current`,
and hosted would hash textures again more often.

**So:** at most about 0.6 ms a game frame on 000 in modern, less what the readers write, for a dozen hooks where
one missed is silent. 009's target
loads after the uploads (3.1 ms) and the table's `sa_own` are larger and smaller risks. If it is built, it wants a
check switch that writes every owed page at once after each upload and compares VRAM, run over the four spots, the
map tour and save states taken and loaded.

### The icy corridor: the ice creatures' shine (branch `perf/icy`, from `main` fd3956b7)

The owner, at 240 (3x, MSAA 2, aniso 2, cut-out AA ssaa, modern sky, widescreen): in the icy corridor of map 017
(`bugs1025/quick_icy`) the F3 counter read `56 FPS / 11 GAME`. The grey is the map's fog, not a screen effect
(SCREEN_EFFECTS.md, 15): nothing reads the frame, nothing is written back or decoded (`--stats`: 0 write-backs, 11
decodes in 700 vblanks, as anywhere).

**What it is.** The creatures in the corridor are skinned models (program 0x280), each part two passes: the first
(ALPHA 0x44, As 0x80: opaque, Z written, kept by the skinned groups) and the sphere-mapped shine, Cs + Cd As with As
up to 146. As above 0x80 is blended in the shader (`sa_draw_batch`'s `as_over`), and a batch blended in the shader is
drawn in runs of triangles that do not overlap, each run a copy of the pixels it blends over (`sa_copy_run.frag`,
conservative, in the pass) between two barriers, then its Z-first draw and its colour. A part's shine is 20-26
triangles folded over each other: 3.5 runs on average. With about 700 parts on screen that is 2,400 runs, 4,900
barriers, 5,700 draws and as many pipeline binds a picture between (the fortress: 4,500 draws, 256 binds). The skinned
groups cannot take these parts: a shine blended in the shader ends the group (`role` 0), so each part's first pass
is a group of one. Measured (`--fast`, every picture between drawn, a8eb108e): the back end played the calls for 15 s
of a 19.5 s run (the fortress: 2.4 of 18.9), 79% of it in the NVIDIA driver: `vkCmdBindPipeline` 24%, the barriers 16%,
descriptor sets 16%, dynamic state 10% (`tools/sampler`, `backend_thread`). Hosted is worse: there each run is a
render pass of its own with an MSAA resolve (`COPY_BY_PASS`): 433 passes and 335 resolves a picture, 5 GB loaded and
stored, 10.9 ms of GPU a picture.

**Why the runs stay.** Two triangles of a shine that overlap where a part folds over its own edge meet at exactly
the same depth on the shared edge; the GS blends both there, one after the other. Drawn whole (one copy for the
batch), only one is: 178 pixels a picture differ (at most 64 levels) at the owner's settings. Telling ahead which
pairs can tie does not help: 90% of the cuts are such folds (tied only on their shared edge), and the CPU's corners
(whole Zs) are not the GPU's (a separation test needed a margin of 8 Z to keep the pictures). Fewer runs by
reordering the triangles into levels would save 18% of them. `KF4_SA_SHINE_RUNS=0` (a test) draws such a shine
whole: 237 pictures a second against 140, GPU 3.5 ms a picture against 5.2. (Now `--native-shine`, whole by
default: "The ice shine drawn whole" below.)

**What changed.** A shine right after its own first pass (the same corners, bit for bit, `sa.first_z`, noted by
`sa_note_first_z` for a two-phase first pass whose alpha test passes every pixel) leaves out its Z-first draws:
the first pass wrote its Z exactly over every pixel of those triangles, so a GEQUAL Z-first draw writes only the Z
already there (`shine_z_there`; `--stats`: "shines blended in the shader over their first pass's Z").

**Numbers** (the owner's settings, `quick_icy` standing still; the GPU idle; a8eb108e against perf/icy):

| | a8eb108e | perf/icy |
|---|---|---|
| `--fast`, every picture between drawn: GS's thread (GIF) a game frame | 16.1 ms | 16.0 ms |
| the same: back end, Vulkan draws, pipeline binds (700 vblanks) | 15.1 s, 13.5 M, 12.4 M | 12.9 s, 9.8 M, 8.7 M |
| the same: wall | 19.5 s | 17.2 s |
| real time (`KF4_PRESENT_SIM=240`, 1,000 vblanks): pictures a second, game frames | 124, 30.0 | 138-142, 29.9 |
| real time: GPU a picture (`KF4_GSN_GPU_TIME=1`) | 5.5-5.6 ms | 5.15 ms |
| every picture asked for (`KF4_INTERP_CUT=0`), 10 s sampled: back end busy, GS's thread busy | 86%, 62% | 76%, 46% |
| the same: pictures between drawn | 2,162 | 2,840 |

The other renderers at the same spot (real time, a8eb108e): hosted (`--renderer native --native-draw on,passes`) 53
pictures a second, the game at 26, 10.9 ms of GPU a picture; the PS2 path (`--native-draw off`) 83, the game at 30.
With the GPU shared with a game the owner played, modern fell to 61-64 pictures a second (the fallback cut to none
for most vblanks), the game still at 30. The owner's 11 game frames a second was not reproduced in modern; hosted is
the nearest (its frames at 30 alone cost the GS's thread 46 ms each in `--fast`).

**What is left:** the runs. Each costs a copy, two barriers and two pipeline binds (the copy's program and the
blend's, whose layouts differ, so the descriptor sets are bound again each time). Options: the copy's program with
the blend's layout (no descriptor rebinds), fewer runs by levels, or the shine drawn whole (the owner's call: the
fold-edge pixels above). The owner chose the shine drawn whole: "The ice shine drawn whole" below.

**Pictures:** byte for byte a8eb108e's: the four spots and the icy spot (every picture of vblanks 200-207, 301, 399;
under water 400-407, 600: 196 pictures), the forced water, drain, copytex, prev and haze at the pond (every picture
of 600-609: 200), the herb run in modern at the owner's settings (the same `--log`, PATH1 hash 561c91020b2908ee with
`--frames 1841`, pictures 1800, 1820, 1840 with every phase). The off check passes: boot_test; frame_test `--renderer
gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0 of 90; herb with `--native-draw off` against
play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1 cycles 460424273, pictures 1800, 1820, 1840 byte for
byte.

### The ice shine drawn whole (branch `perf/shine-whole`, from `main` f2ff69d4)

The owner's call ("let's get what we can out of it"): the shine drawn whole by default in modern, the runs a switch
away, as the rule for the PS2's quirks has it (modern by default, a switch for the PS2's picture).

**What changed.**
- `--native-shine whole|exact` (`settings.json` `display.native_shine`, `"whole"` by default; the overlay's Renderer
  section, beside the native GS's other picture options: "Ice shine: exact PS2 folds (slower)", live). Whole: a second
  pass blended in the shader right after its own first pass over the same corners (`after_first` in `sa_draw_batch`,
  the same test as the left-out Z-first draws above) is drawn whole, not in runs. Nothing else changes: other shines,
  other blends in the shader and the tie runs keep their runs. Exact is main's picture. `KF4_SA_SHINE_RUNS` is gone.
- Single-sampled, such a shine is kept by the skinned groups (`shine_kept`): the group's shines blend from one copy
  made after all its first passes, by all their triangles (the run copy's program, conservative, in the render pass),
  instead of each part's first pass a group of one and each shine a copy between two barriers. The same picture: a
  pixel takes one part's shine at most (the marks), the part whose first pass is the last to draw there, and the copy
  has that first pass's colour there, as the copy before that part's shine had.

**The groups with MSAA too (the default, the owner's choice).** The copy is each pixel's samples' mean. Drawn part by part, a part's copy at a
pixel it shares with another part (a seam: one sample each) has the part before it already shined there, and the shine
is blended from that: the seam comes out brighter. Kept in a group, the copy has the first passes alone. So with MSAA 2
the groups differ from part by part on about 1,165 pixels a picture at the icy spot (up to 76 levels), all along the
parts' seams. Part by part is the artefact: single-sampled (the PS2's way) the seams have no such lines, part by part or
kept, while with MSAA 2 part by part draws a bright dashed line along every seam of the creatures (main has them too,
exact or whole); kept in groups, the creatures look as they do single-sampled. So they are kept with MSAA too by default (it changes
main's MSAA pictures there, along the seams, and removes the lines). `KF4_SA_SHINE_GROUP=1` (a test) keeps them
single-sampled only, part by part with MSAA as before; `KF4_SA_SHINE_GROUP=0`: never kept.

**Numbers** (the owner's settings: `--renderer modern`, 240, 3x, MSAA 2, aniso 2, cut-out AA ssaa, modern sky,
widescreen; `quick_icy` standing still; real time 1,000 vblanks, counted over vblanks 400-980). The owner's game was
running on the GPU for most of these (the GPU 20-80% busy without them), so the real-time numbers are low and noisy;
`--fast` with every picture between drawn (700 vblanks) counts the CPU's work alone.

| | main (= exact) | whole (the default) | whole, kept with MSAA (`KF4_SA_SHINE_GROUP=2`) |
|---|---|---|---|
| `--fast`, every picture between drawn: back end | 12.5 s | 4.9 s | 0.95 s |
| the same: Vulkan draws, pipeline binds | 9.8 M, 8.7 M | 5.0 M, 3.4 M | 3.2 M, 0.30 M |
| the same: GS's thread (GIF) a game frame | 23.7 ms | 15.9 ms | 13.3 ms |
| real time, the GPU shared: pictures a second | 64-66 | 78-92 | 212-236 |
| the same: GPU a picture (`KF4_GSN_GPU_TIME=1`) | 3.8-4.0 ms | 4.5-5.2 ms | 2.8-2.9 ms |
| the same: GS's thread, back end a picture | 3.4-3.5, 1.8 ms | 3.0-3.7, 1.3-1.7 ms | 1.8-2.1, 0.5 ms |
| real time, the GPU nearly idle: pictures a second, GPU a picture | (perf/icy: 138-142, 5.15 ms) | 221, 3.75 ms | 240, 1.87 ms |
| the same: GS's thread, back end a picture | | 1.6, 1.7 ms | 1.4, 0.35 ms |

Single-sampled (MSAA 1, where the groups take the shines by default), `--fast` as above: back end 12.6 s (main) against
0.89 s, pipeline binds 8.7 M against 0.32 M. The sampler (`tools/sampler`, every picture asked for, 10 s): whole
spends 57% of a core in the back end (78% of it in the driver: pipeline binds 20%, descriptor sets 19%, barriers 14%,
dynamic state 12%); kept in groups 11%.

**Pictures** (against main f2ff69d4, the owner's settings, `--fast`): the four spots byte for byte (every picture of
vblanks 200-207, 301, 399; under water 400-407, 600), with MSAA 2 and with MSAA 1. The icy spot (every picture of
200-207, 301, 399, 400-407, 600: 76) whole against main's `KF4_SA_SHINE_RUNS=0` byte for byte, with MSAA 2 (part by
part) and with MSAA 1 (kept in groups); against main, 44 of 76 differ, 103 pixels a picture (at most 64 levels), all on
fold edges, main's brighter (the second blend). `--native-shine exact` byte for byte main's (MSAA 2 and 1), and
`display.native_shine` `"exact"` in `settings.json` (MSAA 2, a hidden window: `KF4_OVERLAY_SHOT` reads the settings). The forced water, drain, copytex, prev and haze at the pond (every
picture of 600-611: 240) byte for byte; the herb run in modern (the same `--log`, PATH1 hash 561c91020b2908ee, pictures
1800, 1820, 1840 with every phase) byte for byte. The off check passes: boot_test; frame_test `--renderer gs` and
`native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0 of 90; herb with `--native-draw off`: the same `--log`
as main, PATH1 hash 204e443a9fc49731, VU1 cycles 460424273, pictures 1800, 1820, 1840 byte for byte.

**What is left.**
- With MSAA: the groups (above), the owner's call. Without them each part is still a group of one, a copy and two
  barriers, and the GPU's time goes mostly to those barriers.
- Part by part, in levels: parts whose places on the screen do not meet drawn as one step (their first passes, one
  copy, their shines) would be exact with MSAA too. By the parts' boxes a group of 9.3 parts is 4.2 levels, but in a
  picture between the GPU moves the corners and the boxes are not known (the marks take every part there for the same
  reason): it needs each part's box over the picture's move, a bigger change.
- The copy's program with the blend's layout (no descriptor sets bound again at each switch: 19% of the back end, part
  by part) changes `gs_in.frag`'s layout (both input attachments), so the shipped pipeline list's lines for it go
  stale; with the groups the back end no longer limits.
- Exact (`--native-shine exact`) is main's runs, untouched (fewer runs by levels: 18% fewer).

### Target loads scaled on the GPU (branch `p8/target-load-gpu`, from `main` bc2f9d9c)

At 240 the pond (map 009) was the one place measured that was bound by the GS's thread: 164 pictures a second, the
GPU 42% idle. It came in bursts. A game frame's packets (about 13 ms) and the recording of its first picture between
(about 10 ms) came within one vblank, and the cut took pictures between away. Sampled
(`tools/sampler/profile.py --map 9 --hz 240`, nothing cut), `load_target_rect` was 15% of the thread: 11 points from
the uploads the recording asks for again (`replay_upload`, `sync_targets`) and 4 from the frame's own. The recording
also kept those bytes, copied them into a buffer (`finish_recording`, 24%, much of it a byte-at-a-time vector
copy) and copied them again each picture.

**Why so much:** `load_target_rect` made the target's pixels at its scale on the CPU, each GS pixel sx x sy times
(24 at 4x with native widescreen), and sent all of them.

**What changed:**
- A scaled target's rectangle is sent at the PS2's size into a staging image. A blit (nearest) then scales it into
  the target. Destination pixel x reads source (x + 0.5) / sx, which is never a whole number, so each pixel lands on
  its own GS pixel: the same pixels as before, with 24 times fewer bytes made, kept and copied.
- In the frame, one staging image is used again load after load. The barriers around each load order them.
- In a recording, each rectangle has a place of its own in the recording's staging images (`Recording::stages`),
  written once as a decoded texture is. The recording makes the blit again (`Cmd::blit_image_recorded`), and the
  back end's `OP_BLIT` now names the targets' images as they are at the replay (`image()`), as the copies do.
- Only formats that can be blitted both ways (`load_scaled_on_gpu`). `KF4_GSN_LOAD_SCALE=0` makes the rows on the
  CPU as before. `--stats` counts the loads scaled on the GPU.

**Pictures:** byte for byte the CPU's. Each map was run with `--fast` and every picture between drawn
(`KF4_INTERP_DRAW_ALL`), at the owner's settings (4x, native widescreen), from the teleport to 000, 008, 009, 022 and
063. Every picture of vblanks 2300-2303, 2450-2451 and 2600-2601 was dumped (160 pictures, 31,000-79,000 loads on
the GPU a run). The same on 009 and 063 with MSAA 2 and `--native-colour high` (64 pictures), on 009 hosted
(`--renderer native`) and on the PS2 path (`--native-draw off`; 64 pictures, about 79,000 loads each), and after a state
saved on 009 and loaded (12 pictures). On this GPU both formats in use blit both ways (RGBA8 and D32), and the depth
loads took the GPU too. `frame_test.py --renderer native`, `boot_test.py`, gsn_draw, gsn_vram and vu1_test pass.
- **Not run:** the map tour (no captures here), a Linux build, a live `--renderer both`. The blit's exactness was
  checked on NVIDIA only. The argument above holds for any driver that follows the spec's nearest rule.

**Numbers** (`pace_test.py --hz 240`, RTX 4090 and i9-13900KF, CPU rows -> GPU scaling, two rounds alternating on
009):

| map | pictures a second | lowest second | vblanks with none between | GS's thread a game frame | a recording |
|---|---|---|---|---|---|
| 009 | 161, 210 -> 235, 236 | 60, 159 -> 222, 220 | 120, 0 -> 0, 0 | 25.4, 22.8 -> 15.9, 15.7 ms | 11.0, 9.8 -> 5.3, 5.3 ms |
| 000 | | | | 20.0 -> 17.7 ms | 7.0 -> 4.7 ms |
| 063 | | | | 15.2 -> 14.5 ms | 4.9 -> 3.5 ms |

The GPU's time a picture is the same (3.2-3.4 ms on 009). The pond now keeps up at 240, its GPU about 78% busy.

## The numbers (first round)

Every run headless, `--no-audio`, a fresh data folder, at most 2 games of mine at a time. Other agents'
games were running on the PC, so the timings are noisy. Hosted means `--renderer native` with the same
kinds. Pixels differing, hosted against modern; the evidence (sheets: hosted | modern | heat x8, and the
reports) is in the session's scratchpad, `nd5_p8/evidence/`.

| scene | kinds | scale | result |
|---|---|---|---|
| herb (`herb.pad`, 1800-1840/20) | map,sky,items | 1x | 33 pixels in 3 pictures (worst 47: edges) |
| 000 walking at 240 (`walk.pad`, 1900-1906, every picture: 7 frames and 21 between) | map,sky,items | 1x | about 530 a picture, frames and pictures between alike: silhouette edges against the sky; the pictures between move correctly |
| 009 start (`newgame.pad`, `--teleport 1600:9:0`) | every kind | 1x | 4 pixels |
| 009's water and the miners (`m9w`) | every kind | 4x | 1,679 in 2 pictures |
| 016's bats | every kind | 1x | 0 |
| the fight on 000 (`strong0`, `strong3.pad`) | every kind | 1x | about 1,270 a picture, all but about 150 by 1 level (worst 40) |
| bugs1022 `quick_teeth` (the lava's horizon on 000) | map,sky,items | 4x | 2,004: **hosted has notches of black along the lava's far edge, modern a straight line** |
| the same, 3x, MSAA 2, native widescreen, cut-out AA | map,sky,items | 3x | 2,547: the same notches (hosted's) |
| `quick_tower` (the tower's top seen from below) | map,sky,items | 4x | 7,990: the rock layer over the tower's top a little different where the meshes are far apart in Z (hosted's slope rule kept) |
| `quick_brick` | map,sky,items | 4x | 14,072: a seam line at the floor and a patch on a tower's top |
| `quick_lacing` | map,sky,items | 3x | 61,685, of which 6,509 more than 1 (worst 33): blend rounding over the arches; no lace in either |
| bugs1024 `quick_bridge` | map,sky,items | 4x | 1,027 (worst 24) |
| blink `door.kf4quick` | map,sky,items | 4x | 432 (worst 23). The blink is in motion; one still picture cannot show it |

- **The tie-break artefacts.** The ones the GS's rounding made (lace, teeth, hatching, the lava's
  sawtooth) cannot occur in the scene's draws: their Z is never rounded. What is left of the ties is the
  slack rule. Before the slope term and the lava's blending were in, the first tries showed what goes wrong
  without them: stripes along a wall's overlay (no slack), and black teeth along the lava's far edge (the
  lava drawn by the GS from integer vertex Z against the scene's exact hills).
- **The off check passes:** boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300;
  vu1_test 0 of 90; herb with `--native-draw off` against play-932801f: the same `--log`, PATH1 hash
  204e443a9fc49731, VU1 cycles 460424273, pictures 1800, 1820 and 1840 byte for byte.
- **GPU time** (the far save, 4x, MSAA 2, native widescreen, at 30, 1,000 vblanks, `KF4_GSN_GPU_TIME=1`):
  hosted 2.05 ms a picture, modern 2.80. The difference is the turns' copies (copy 0.47 ms, multisampled
  loads and depth 0.29 ms). At 240 both runs fell behind the game on this busy PC (two other games
  running), so the 240 numbers are not comparable yet. Turns cost about 0.33 ms each at 4x MSAA 2: they
  are the price of islands, and they go away as kinds move into the scene.
- **Not seen yet:** save states loaded across modes (a quick load works in modern; a `--state-load` taken
  hosted and loaded modern was not tried), Linux and lavapipe, the six wave-UV maps in modern, the
  under-water and MP-drain passes over the scene, the King's Map (its own loop, not tried).

## How to work on P8

- Everything in NATIVE_DRAW_TASKS.md, "How to work on native draw".
- **Compare:** `python tools/native_draw_compare.py --mode modern --kinds K ...` (hosted against
  modern, the same kinds). For a quick save, use `--input` with a pad that presses CROSS on the title
  (the review's `title.pad`: `100 CROSS 4`), then `-- --quick-load FILE`. Pictures between are dumped
  as `frame_N_250.png` etc.; the tool compares only the frames.
- **Debug switches** (read at the vblank that turns modern on): `KF4_SA_KINDS=MASK`, `KF4_SA_LOG=1` (each
  batch's FRAME, ZBUF, TEST, ALPHA and Z range, and why one is declined), `KF4_SA_PIXEL=X,Y` (the scene's
  batches over a GS pixel: the triangle's Z, TEX0, the blend, the slack; the pixel's Z and q interpolated, the Z's
  change across a GS pixel), `KF4_SA_SLACK=LAYER[,TIE]`, `KF4_SA_WORLD=EPS` (a layer's world units; 0: the Z rule), `KF4_SA_SKIN=EPS` (a skinned
  model's parts' tie in world units; 0, the default: exact), `KF4_GSN_DYNAMIC=0|1` (no dynamic state, or none for blending: "Pipeline binds"), `KF4_SA_GROUP=0` (a skinned model's parts drawn as they come,
  not kept: "A skinned model's parts drawn together"), `KF4_SA_RUN_COPY=0` (shader blending's copy over the run's box, not by its
  triangles), `KF4_SA_SHINE_GROUP=0|1` (a shine drawn whole never kept by the skinned groups, or kept single-sampled only; 2, the
  default: kept with MSAA too: "The ice shine drawn whole"), `KF4_SA_REFLECTION_KEEP=0|2` (the frame's reflection drawn in every picture between, or always kept; 1, the
  default: kept while the camera is the frame's bit for bit: "The pond at 240"), `KF4_SA_ZFIRST=0` (no Z-first draw by depth
  alone), `KF4_GSN_GPU_TIME=2` (the GPU's time a picture by what is drawn), `KF4_ND280_PLAN_CHECK=1` (each 0x280 plan made both ways, those
  that differ counted in `[stats] native draw 0x280 plans`),
  `KF4_SA_NO_TABLE=1`; the table's (read once): `KF4_SA_VERIFY=1..4`, `KF4_SA_PACK=DIR`,
  `KF4_SA_PACK_DUMP=DIR`. `KF4_SA_LOG=3` (T2): the GS's batches the scene draws, and those it draws elsewhere.
- **The scene at its own scale** (T2): `native_draw_compare.py --mode modern --scale 1 --first-args='--native-scale 4
  --native-msaa 4' --second-args='--scene-scale 4 --scene-msaa 4'`.
- **The map tour in modern.** `map_tour.py --renderer modern` (T6; the T4 runs changed its commands by hand). A modern run ended in a broken heap at its exit
  (0xC0000374 or 0xC0000005, after the last picture, 009 every time from a quick load): `sa_draw_begin`'s
  fetched meshes were a function's static, made on the GS's thread after main's `atexit(print_stats)`, so
  destroyed before it while the GS's thread still drew. They are `sa.fetched` now (T4). Native/draw
  e8a89c00's tour in modern: 19 of 64 maps ended so; T4's: none.
- **T4's tour in modern** (64 maps, 8 views each, 1x): its 508 pictures byte for byte those of e8a89c00
  (the CLUT and textures from the table change nothing where the table is exact). The scene's textures:
  900,486 from the table, 11,604 render targets (T3), 495 drawn or copied over (the in-place scroll on
  008, 018 and 097: T3), 49 not uploaded (103's psm 0x10); its CLUTs: 900,295 the table's, 0 the GS's.
- **Threshold.** Modern is not the PS2's picture bit for bit: the ties are the slack rule, not the GS's
  rounding, and some blends round differently. The bar is the owner's eye on the screenshot sets: no
  blobs, no missing or extra surfaces, no stripes or teeth. Differences are expected along silhouettes,
  at seams, and where hosted itself shows a GS artefact.
- **Ownership.** `gsn_standalone.inc` and the `sa_*` shaders are P8's. Hooks in `gsn.cpp` and
  `gsn_native_draw.inc` stay one line each; a task that needs a new hook says so in its report.

## The remaining P8 tasks (for up to 8 agents)

Serial points: **T1** (the scene draws every program) comes before T2, and T3 (the reflection) needs
T1's 0x280 for the entities in it. T4, T5, T6 and T8 can start at once. T7 reviews at the end.

| # | task | owns | acceptance |
|---|---|---|---|
| T1 | **Every program in the scene.** 0x280 (entities, the arm, gold, jointed objects: `nd_280.vert` with `ND_STANDALONE`), 0x700 (effects, with their TEX0 swap), 0x5A0 (shadows: a stencil, first write wins, instead of DATE/FBA, A17), the trail strip, posed objects and the picked item (0x000: `KF4_SA_KINDS` already draws them). The depth clear before the picked item and the arm becomes a scene clear (`GsDraw_ClearDepth` as a draw kind, or the GS's sprite met as a scene op). | `gsn_standalone.inc` (the batch path: one writer), `shaders/nd_280.vert`, `nd_700.vert` (their `ND_STANDALONE` blocks), `gsn_nd_280.inc` and `gsn_nd_700.inc` hooks | every ported kind in the scene: turns down to one or two a frame on 000 and 009; hosted against modern on the review's sets (000, 009, 016, the fight, the far save, spells 3, 36 and 66, `m8w`/`m9w`) at 1x and 4x: no blobs, only edges; shadows each pixel darkened once; GPU time at the far save no worse than hosted |
| T2 | **The scene's own end of the world.** With every kind in the scene, one turn a frame: the scene clears itself to the background colour (no acquire), and is given to the GS once, after `rd_end_world` and before the passes and the 2D. Then the composite becomes a real one: the GS's 2D drawn into a transparent layer and blended over the scene (premultiplied), so the scene can be at a resolution and sample count of its own (`--scene-scale`, `--scene-msaa`, independent of the GS's). The passes (P7's `rd_pass`) become scene shaders reading the scene's colour (and depth for haze). | `gsn_standalone.inc` (the turns), a new `gsn_composite.inc` | modern at 4x MSAA 4 with the GS at 1x: the HUD, the messages, the menus and the title the GS's, the world 4x; every P7 pass (under water, aiming, fades, brightness 0x40 and 0xFF, the MP drain) matching hosted within levels; GPU time below hosted's |
| T3 | **Render targets by name.** The reflection (mode 2's 256 x 256 CT16S at 0x2BC0, at 256 x scale or full resolution: A9), the scrolling textures (`MapScroll_UpdateDraw`'s 128 x 128 targets, `ScrollTexture_DrawAll`'s four at 0x3A40) and the frame copies (`GsDraw_CopyFrame` 0x2BC0, `GsDraw_CopyFrameToTexture` 0x3540, the menus' backdrop 0x3240) as scene images that own their pages in the table, and a texture on those pages read from them. `MapScroll_UpdateCopy`'s in-place scroll (a local copy) as a texture offset, or a scene copy. Mode 1's mirror (041) in the scene. | `gsn_standalone.inc` (the table's target side), a new `gsn_scene_targets.inc` | `[stats] standalone` shows no "a render target" or "drawn or copied over" textures on 000, 008, 009, 014, 041; the reflection on 009 at 240 walking and turning; the clouds and the lava scroll seamless with `--seamless-scroll` |
| T4 | **The CLUT and the rest of the table.** CLUTs from the table (mirror the GS's CLUT buffer: CLD 0-5, CSM1 and CSM2, CSA, from the chunks), PSMT8H/4HL/4HH textures, the 24-bit uploads' blocks (they own part of each word), and a block shared by more than 4 uploads. `--stats` counters per map of what is still the GS's. Texture packs keyed by chunk hash (a dump and a load folder) as the mods' groundwork. | `gsn_standalone.inc` (the table: "Who owns VRAM" and "Textures from their source") | the 64-map tour in modern: every scene texture from the table except render targets; the font's two sheets by CLUT (0x3FE0/0x3FE1) right when items' names show; a texture pack that recolours one wall texture |
| T5 | **The sky modern (owner's decision 3) in the scene.** `--sky modern`: the sky's records drawn first at the far plane (depth 0, reversed Z), centred on the eye, with their own fog. Before making it the default, measure P0-3's four maps (002, 004, 013, 027) where geometry behind the dome would show (3.6%, 0.7%, 2.8% and 0.3% of the corners sampled), and look at them. If that geometry looks wrong when shown, that is the solid reason to report; a fix is to draw it with the sky's depth (cut by the dome's radius). | `game/native_draw/nd_sky.c` (the flag), the sky path in `gsn_standalone.inc` | the sky sets at 240 (walking, turning, looking up and down) on 000 and 013; pictures of 002, 004, 013, 027 hosted against `--sky modern`, for the owner |
| T6 | **Save states, captures and replay.** The scene's state is derived (rebuilt from the chain), but the table is not: after a state load the textures come from the GS until they are uploaded again (`sa_forget`). Check states taken hosted and loaded modern and back; carry the table in captures (`.kf4gs`) so `kf4-gsreplay --renderer modern` replays the scene; the map tour in modern. | `port/debug/gs_capture.c` (records), `tools/gsreplay/`, `gsn_standalone.inc` (`sa_forget`, the table's save) | 20 states across modes with no wrong textures after the load; the 64 tour captures replayed modern, their scanouts compared with hosted |
| T7 | **Review and the owner's sets.** An independent review of `gsn_standalone.inc` and the hooks: races (the turns against every path that touches a target; `ring_idle` callers from other threads), the pictures between (recordings with turns in them), MSAA and colour-high, widescreen, `--native-layer-z ps2` (rounded release). The screenshot set for the owner at his settings (3x, native widescreen, MSAA 2, cut-out AA, 240): the spots in bugs1022 and bugs1024, hosted and modern, still and in motion (strips of the pictures between). | read-only | the review's findings ranked; the sets in the owner's index |
| T8 | **Performance, and Linux.** Time at the owner's settings at 240 on an idle PC (hosted against modern, `KF4_GSN_GPU_TIME`, `KF4_PRESENT_SIM=240`). The turns: copy only the rows drawn on acquire too, or draw the scene straight into the GS's images while they are the scene's (no copies), until T2 removes them. The tie rule's Z-first passes: batch them, or use `gs_zonly`-style programs. Linux build with gcc and lavapipe: `--renderer modern` on the herb run. | `gsn_standalone.inc` (the turns' copies, with T1's agreement), CI | modern no slower than hosted at 240 at the owner's settings; the herb run on lavapipe, its pictures against Windows' within edges |

### T5: the sky in the scene (done, branch p8/t5)

- **Far-plane skies (002, 013, 021, 027, 098, 099).** `sa_kind` now takes `RD_DRAW_FAR_Z` draws, so the modern
  sky is drawn into the scene, not by the GS between turns. `sa_000.vert` gives a far corner a depth of 0
  (`zf = 0`, the scene's clear value). Before this, only the GS's Z was 0. The dome's TEST stays Z ALWAYS, so it
  lays down the far plane wherever it is drawn (013's frame is not cleared under the sky).
- **000 and 004, `--renderer modern` only** (`nd_sky.c`). The dome is put at the eye, uses its own Z (the scene's,
  unrounded) and keeps the game's TEST (GEQUAL, Z written). It is drawn where the game draws it. Hosted mode
  keeps the PS2's dome, unchanged. On 000 the dome adds its clouds (ALPHA `0x48`, Cs * As + Cd) over the canyon's
  silhouettes beyond it. With real depth those stay soft dark shapes under the clouds, as on the PS2. Only the far
  plane turns them into black cut-outs. So no stencil or second pass is needed for them. The note in
  NATIVE_DRAW_TASKS.md that the eye with real Z also gives black shapes did not reproduce: hosted and modern
  at the eye both keep the shapes soft.
- **What the dome still hides.** It hides what the PS2's dome hides: pieces of `Map_DrawLateRecords`' records
  (drawn after objects, entities and items) that lie beyond the dome. On 000 that is 6 of 64 views, all at
  2260,-5200 looking up, up to 4,163 pixels at 1x. On 004 nothing hides in 64 views (the eye against the far
  plane: 0 pixels). Showing those pieces under the clouds needs the dome drawn after them. A try at that (the
  sky's batches held until the end of the scene's turn) did not help yet. A PS2-path draw (an additive object,
  Z written) comes between the sky and the late records and ends the turn, and an additive Z-writing draw
  must see the sky first anyway. It becomes possible once T1/T2 leave one turn a frame.
- **Numbers** (1x, 64 views a map: 8 places x 4 headings x pitch 0 and 0.6). Hosted `--sky modern` against
  modern: 002 3.1M pixels, 013 5,283, 027 4.4M. The far-plane maps' large counts are 1-2 levels all over
  the sky (on average +0.5 a channel), because Vulkan's blending rounds to nearest where the GS truncates (P8's
  known blend rounding, in the batch path). The hosted PS2 dome against modern on 000 is 7.7M (the clouds from
  220 units higher) and on 004 0.76M. At 240 (walking, turning, strafing, looking up and down, 128 pictures a
  map on 000 and 013), modern's step from picture to picture matches hosted's to within 2% in every view: no
  jumps.
- **Checks.** The off check is the same as before. Hosted `--native-draw on` and `--sky ps2` on the herb run
  are byte for byte e8a89c00's. Modern's herb pictures are the same too (no sky in them). Long modern runs with
  many quick loads still exit with 0xC0000005 or 0xC0000374 after printing their stats. e8a89c00 does the
  same (T8's open item).

**What the owner decides for P8:**
- The tie rule's look against the PS2's. Modern draws ties by the slack rule, so seams and layer edges
  can sit a fraction of a pixel from hosted. This is the cost of having no tie-break artefacts.
- The scene's resolution and samples apart from the GS's (T2): whether the HUD and menus stay at the
  GS's scale, and what `--native-scale` means in modern.
- A9 (the reflection's resolution and widescreen projection) when T3 makes it a scene target.
- The sky (decision 3), after T5's pictures of the four maps.

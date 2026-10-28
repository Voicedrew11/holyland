# The PC renderer (`--renderer fast`)

A real PC renderer for King's Field IV's world: the map, the map objects, the items, the characters and the arm
drawn by our own Vulkan renderer from the game's data and state, the way a PC game draws its world, instead of
the PS2's GS emulated draw by draw. The GS path keeps what is 2D or over the whole screen (the HUD, the menus, the
text, the fades and the screen passes) and, until they are ported, the effects. `--renderer modern` stays the
reference (the PS2's pictures, checked byte for byte); `fast` is checked against it by how much it differs, not
whether, and is free to differ where that is the point (filtering, depth, anti-aliasing, frame rate).

Status: the plan, agreed with the owner (October 2026); P1 under way. The owner's choices:
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
- **P2: the snapshot and the map.** The `RD_ND_WORLD` mark and the frame's snapshot; the PC renderer draws the map
  records (program 0x000, opaque and alpha-tested) into its own colour and depth, composited at the mark; every
  other world draw still goes to the GS that frame (the turns as now) so the game stays whole while the renderer
  grows.
- **P3: the whole world.** Blended and layered records, map objects, items, gold, entities (0x280, skinned), the
  sky, the shadows, the arm and what is held (the eye's view), the picked item. The GS then draws no world.
- **P4: reflections.** The mirror (mode 1) and the offscreen reflection (mode 2) as views of the same scene; the
  rippled water (`RD_PROGRAM_RIPPLE`), the scrolling textures.
- **P5: pictures between from snapshots.** The world's pictures between by interpolation; the recording keeps
  only the GS's part.
- **P6: effects.** Program 0x700's draws as instanced particles; then the GS keeps only the 2D and the passes.

## The first interface: the frame's snapshot (P2, for agreement)

On the game's side (`port/include/port/render.h`, beside `rd_draw`), so that the adapters change in one place:

```c
/* ---- The PC renderer's frame (--renderer fast, docs/port/PC_RENDERER.md) ----------------------------------------
   The world's draws go to the PC renderer as the frame's snapshot instead of the GS's chain: rd_draw still makes
   each one (its meshes, camera, constants, identity), and rd_world_end hands the frame's to the renderer at the
   world block's end, with the chain's mark of where the world goes (RD_ND_WORLD). The game's thread only. */

/* The views the world is drawn in, in the game's order (GameLoop_DrawFrame): RdDraw.camera's view */
typedef enum {
    RD_VIEW_WORLD,     /* the camera's mode 0 */
    RD_VIEW_MIRROR,    /* mode 1: the world mirrored, under the map's tint (map_draw_state.mode 1) */
    RD_VIEW_OFFSCREEN, /* the reflection drawn into the offscreen target (map_draw_state.mode 2) */
    RD_VIEW_EYE        /* mode 2: before the eye (the arm, what is held) */
} RdView;

/* 1 when the frame's world goes to the PC renderer (--renderer fast): the adapters then call rd_draw and leave the
   chain alone (no RD_ND_DRAW queued) */
int rd_world_to_renderer(void);
/* The world block begins: draws until rd_world_end are its own */
void rd_world_begin(void);
/* The world block ends: the frame's world draws to the renderer, and the mark to queue in the chain where its
   picture goes (PORT_GS_REG_NATIVE_DRAW's value, RD_ND_WORLD) */
uint64_t rd_world_end(void);
```

and on the renderer's side (`port/render/pc_renderer.h`, the native GS's thread):

```cpp
// The PC renderer: the world of a frame's snapshot drawn into its colour and depth, at a picture's time
class PcWorld;  // (the meshes on the GPU, the materials, the views' targets: private)
// the frame's snapshot, met at its RD_ND_WORLD mark: kept, with the one before it
void pc_world_frame(PcWorld &w, uint32_t frame_serial);
// the world at time t (0: the frame before, 1: this frame) into the scene's colour and depth, for the GS's frame
void pc_world_draw(PcWorld &w, float t, const PcTargets &into);
void pc_world_forget(PcWorld &w); // a map change, a save state loaded
void pc_world_print_stats(const PcWorld &w);
```

## Open question

- **P1 under `modern` too** (`--native-sampler hardware`): the plan builds it so, so every map is faster before
  P2-P6; its pictures differ from the PS2's in filtering only. Not decided: `fast` uses it either way.

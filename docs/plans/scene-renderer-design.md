# The scene renderer: its design

2026-11-19. The design `HANDOFF_scene_renderer_design.md` asked for: the renderer that replaces today's PC renderer
(`port/gs/pc/`, a replay of the GS's stream), settled enough to be built in slices without deciding it again. The
direction and the owner's decision are `renderer-scene-not-gs.md`; the studies behind it are
`camera-whip-slowdown.md`, `fullscreen-layers-inventory.md`, `world-plain.md` and `depth-by-layers.md`. This step
added developers' switches and counters only (below, "What this step added"); nothing a player gets changed.

## The answer, short

- **A frame is already nearly a scene.** In play a game frame hands the renderer 92-708 native draws (660-3,460 VU1
  runs, 12,000-44,000 triangles) and only 140-460 GIF primitives: the two swap clears, the depth clear, the alpha's
  reset, the scrolling textures' sprites, the HUD (about ten sprites), the screen passes, and 63-333 triangles of
  program 0x000 that native draw does not take yet. The scene is the native draws with their materials resolved in
  the chain's order, the GIF draws as 2D items, the targets as passes, all in the stream's order ("1. The frame as a
  scene").
- **Depth: `D16_UNORM`, not the GS's arithmetic in the shader** (the owner's decision, 2026-11-19). Measured as the
  ceiling the handoff asked for (`KF4_PC_DEPTH=gsexact`: exact Z's whole Z alone at each pixel, written by the shader),
  the GS's arithmetic as the PC renderer has it is **further** from paraLLEl-GS than `D16_UNORM` (9,371 pixels over 24
  levels in the 760-view tour against 7,008; in motion 1,125,827 against 1,120,237) and its world costs 1.4-3.7 ms more
  a game frame at 2x (4.25 / 9.84 / 8.50 ms at 099, the pond and the palace against 2.85 / 6.11 / 6.22). **At most 33
  of UNORM's 7,008 pixels are depth at all**: no depth mode (the rule, gsexact, the GPU's, the GPU's clamped at each
  pixel) draws more of them as paraLLEl-GS does; the other 6,975 (5,549 of them in one view
  at 099) are something else ("4. Depth's exactness and DATE").
- **DATE without a stencil, exactly**: the shadows' DATE is "each pixel darkened once, by the first shadow drawn
  there" (no shadow writes Z), which the GPU makes order-free: each shadow draw writes its number into a small image
  with a MIN blend, one pass then darkens the pixels by the first one's colour and sets the alpha bit. The HUD's one
  DATE sprite reads the frame's alpha from a copy of its own rectangle. The alpha's reset folds into the frame's clear.
- **Materials: the texture store stays the resolver**, run in the chain's order on the renderer's thread as today; a
  draw keeps the handles it resolved (a bindless index, a CLUT, a sampler), so pictures between ask for no upload
  again. A frame uploads 2.6-7.7 MB, nearly all of it bytes the store already holds (at the pond: 48,197 uploads, 3.2 GB
  over the run, into 736 distinct chunks; 345 decodes for 180,310 resolves).
- **Draws: in the stream's order, batched by runs.** The frame's 860-4,470 draws fall into 190-1,520 runs of one
  pipeline; with textures from a bindless table, a run is one indirect multi-draw. Most of the runs left at the pond
  and the palace are program 0x280's: each of its batches is followed by its sphere pass in another state (927 of the
  pond's 1,020 runs, 1,437 of the palace's 1,523). Drawing a batch's two passes as one draw (the sphere pass blended
  over the first in the shader, exact where the first is unblended) would leave an estimated 100-250 calls a frame.
- **Pictures between from the scene**: the frame's scene drawn again at the picture's time (camera, instance moves,
  2D moves), with no replay of the stream, no uploads again and none of the pump's surface copies. Mouse look is the
  picture drawn with the newest camera (cheap once a picture between is), with a reprojection only as the last
  millisecond's late turn.
- **Built beside today's renderer** as `--renderer scene`, in six slices, each a renderer the developers can run and
  judge against paraLLEl-GS and against today's ("10. The slices"). Today's renderer goes when the scene renderer draws
  the layer inventory's every scenario and the tours and motion cases at least as close to paraLLEl-GS.

The owner's decisions (2026-11-19: the recommendations taken) are in "The owner's decisions" below.

## What this step added (developers' only)

- **The frame census**, `KF4_PC_FRAME_CENSUS=FILE` (`port/gs/pc/pcr_frame_census.h`), with
  `KF4_PC_FRAME_CENSUS_AT="A-B,..."` (the game's vblanks): what each frame hands the renderer, one JSON line a frame:
  bytes by path; GIF primitives by path (1, 2, 3, or made inside a native draw: the passes, the trail) and class with
  their pixels; native draws by kind and program with their VU1 runs, triangles and sphere passes, meshes and cameras;
  the draws in the stream's order and their runs of one pipeline, of one pipeline and texture, of one target; what
  changes between consecutive batches of program 0x280; uploads by destination, copies, read-backs, marks; the
  canvas's own counts over the frame. `tools/scene_census.py` plays the scenes (layer_inventory.py's scenarios),
  headless at 30 with native draw on at 2x, and writes `census.md` and `census.json`. (The title and the menus' loops
  give their pictures no game vblank; their frames are counted whole and those without native draws kept.)
- **`KF4_PC_DEPTH=gsexact`**: exact Z's whole Z alone at each pixel (the plane's fine part dropped, no decal settled,
  no depth rule), written by the shader as the depth; ties then tie in the GPU's own GEQUAL; DATE by the stencil as
  today. `pcr_prim.h`'s `pcr_whole_depth` takes it from `PCR_F_PIXEL_CLAMP`, which means nothing else with exact Z.
  `world_plain.py --variants gsexact`.
- `Canvas::observe_native_batches` (the census's view of each native batch with its registers).
- Raw output: the owner's `~/Documents/darker-reality-measurements/2026-11-19-scene-design/` (its README).

## 1. The frame as a scene

### The census

Each scene's median game frame (headless, 30 game frames a second, native draw on, 2x with native widescreen;
`tools/scene_census.py`). "Draws in order" counts a run of GIF primitives of one GS state as one draw and each native
batch (one VU1 run) as one; a "pipeline" is the GS state less the texture (FRAME, ZBUF, TEST, ALPHA, PRIM's
attributes, TFX, filtering, FBA, PABE, COLCLAMP, DTHE, the native program).

| | 099's start | the pond (009) | the palace (022) | combat (002) | under water (029) |
|---|---|---|---|---|---|
| frames counted | 9 | 61 | 111 | 81 | 11 |
| bytes PATH1 / 2 / 3 | 65 K / 2.79 M / 480 | 115 K / 7.71 M / 480 | 91 K / 4.15 M / 480 | 119 K / 2.59 M / 480 | 12 K / 4.15 M / 480 |
| native draws (not passes) | 160 | 708 | 415 | 179 | 92 |
| native screen passes | 3 | 2 | 2 | 2 | 4 |
| native batches (VU1 runs) | 1,109 | 2,714 | 3,461 | 1,385 | 662 |
| native triangles | 16,304 | 44,265 | 41,383 | 17,227 | 11,997 |
| meshes, cameras | 1,057, 1 | 1,068, 2 | 1,973, 1 | 1,371, 1 | 640, 1 |
| GIF primitives | 141 | 275 | 322 | 464 | 190 |
| draws in order | 1,208 | 3,286 | 4,474 | 1,895 | 860 |
| runs of one pipeline | 207 | 1,020 | 1,523 | 301 | 188 |
| runs of one pipeline and texture | 225 | 1,044 | 1,655 | 317 | 202 |
| runs of one target | 5 | 3 | 4 | 3 | 4 |
| pipelines, textures | 33, 39 | 33, 40 | 28, 38 | 28, 35 | 26, 27 |
| program 0x280: batches, runs | 280, 139 | 1,086, 927 | 2,006, 1,437 | 264, 247 | 146, 146 |
| uploads (MB) | 55 (2.79) | 128 (7.69) | 66 (4.14) | 66 (2.58) | 45 (4.15) |
| local copies | 0 | 6 | 0 | 0 | 4 |
| DATE: native fragments (2x) | 0 | 0 | 12,159 | 28,824 | 1,854 |
| today's canvas: batches, native draw calls, passes | 244, 263, 9 | 1,120, 1,313, 6 | 1,765, 1,826, 7 | 351, 372, 6 | 205, 206, 10 |

By kind (draws / VU1 runs / triangles; sphere passes in brackets):

| kind, program | 099 | pond | palace | combat | under water |
|---|---|---|---|---|---|
| map record, 0x000 | 47 / 474 / 10,548 | 26 / 646 / 15,214 | 55 / 950 / 20,814 | 44 / 516 / 11,561 | 10 / 374 / 8,927 |
| map object, 0x000 | 29 / 239 / 2,690 | 61 / 498 / 5,607 | 41 / 469 / 4,626 | 15 / 421 / 2,291 | 5 / 23 / 146 |
| map object, 0x280 | 7 / 252 / 1,126 | 17 / 44 / 685 | 18 / 545 / 2,658 | 2 / 19 / 177 | 24 / 64 / 289 |
| item, 0x000 | 3 / 33 / 699 | 26 / 350 / 4,752 | 23 / 363 / 3,945 | 5 / 181 / 1,073 | 3 / 97 / 788 |
| entity, 0x280 | 33 / 70 / 1,051 (69) | 328 / 696 / 11,064 (504) | 84 / 770 / 4,302 (718) | 46 / 78 / 1,187 (77) | 24 / 74 / 1,423 (73) |
| shadow, 0x5A0 (DATE) | 33 / 33 / 150 | 164 / 352 / 5,552 | 84 / 254 / 4,344 | 46 / 49 / 266 | 24 / 28 / 414 |
| effect, 0x700 | 8 / 8 / 40 | 84 / 92 / 540 | 110 / 110 / 694 | 2 / 2 / 10 | 2 / 2 / 10 |
| arm, 0x280; the trail | | | | 18 / 118 / 658 (116); 1 strip | |
| the reflection's water (ripple) | | 2 / 36 / 851 | | | |
| screen passes | brightness, fill (099's script), fade's registers | brightness, fade | brightness, fade | brightness, fade | brightness, tint, fade, wobble |

What still comes as GIF in play, a frame (GS pixels; a sprite's rectangle, a triangle's half its rectangle):

| what | path | primitives | pixels |
|---|---|---|---|
| the swap's two clears (`sceGsSwapDBuff`) | 3 | 2 sprites | 573,440 |
| `GsDraw_ClearDepth` (and the reflection's, at 009) | 1 (VU1 0x276) | 1-2 sprites | 286,720 each |
| `GsDraw_SetScreenAlpha(0)` and one HUD sprite like it | 1 | 11 sprites | 291,640 |
| the scrolling textures' sprites into their targets (the sky, water, lava) | 1 | 3-8 sprites | 49,000-131,000 |
| the HUD | 1 | 8-9 sprites, one of them DATE | 25,000 |
| the screen passes (`pass_draw.cpp` makes them GIF again): the brightness's 10 strips, a fill, the wobble's 140 triangles | native | 10-160 | 286,000-860,000 |
| the weapon's trail (combat) | native | 4 triangles | |
| 3D of program 0x000 that native draw does not take | 1 | 63-333 triangles (91 more unblended in combat) | |

The last row is the map's triangles that cross the GS's range, cut up (identified in slice 1, 2026-11-19): where a
map record's box is partly out of the range the GS can draw (`Map_ClassifyRecordOnScreen` 0: a corner projected past
z 0xFFFF0, or outside the 16-bit screen), `Map_DrawRecords` calls `ModelScreen_CutClippedTriangles`
(`model/model_screen.c`) after the record's draw. It projects every triangle on the EE and queues each one with a corner
out of range and some of it on the screen as 16 smaller ones (`ModelScreen_QueueCutTriangle`: four strips of nine
vertices made on the EE, the block's GS setup and the matrix sent along) through VU1 program 0x000. The native draw of
the record leaves those triangles out as the PS2 does, so the pieces come as PATH1. At 099 (base save, 700 vblanks,
289 game frames) 5,264 were queued: about 18 a frame, some 290 triangles, the 63-333 above. In combat the 91 more
unblended ones are the same, the player close to a wall. They go on as GIF: the scene renderer draws them as the
scene's 3D items with `pcr_prim`'s arithmetic (the 1x tour shows them byte for byte the PC renderer's). Drawing them
natively would need the native draw to take vertices the EE makes each frame, for a few hundred triangles, so it is not
worth it before batching (slice 3) wants every 3D draw in the arena.

Outside play (the same counter, the title and the menus' loops; each kind of frame and how many of the run's):

| loop | frame | primitives | uploads | runs of one pipeline and texture |
|---|---|---|---|---|
| the logos | a logo, faded | 4 | 2 (786 KB) | 4 |
| the opening movie | an MPEG frame uploaded and drawn | 16-22 | 18-30 (3.1-3.2 MB) | 12-18 |
| the title, Press Start | text | 452 | 0 | 14 |
| the menu's pages | the backdrop, the shade, the page's text and icons | 52-452 | 0 (2 with 262 KB on a page change) | 18-30 |

These have no 3D, a 32-bit Z buffer (ZBP 140, PSMZ32) and 640 x 224 field buffers; their frames are tens of draws.

### What a game frame hands the renderer, end to end

In the order the stream has it (the census's marks and `frame_interp.c`):

1. `RD_ND_FRAME` (the frame's serial; the side buffer's slot), at 30 a game frame. With pictures between on, the
   frame's marks (0x7D: 0xF0-0xF5 around replays and the frame shown kept; the cameras' and the world's marks that
   `part_moves` reads).
2. PATH3: the swap's draw environment and its two clears (both buffers' FRAME, Z 0).
3. PATH2/3: texture uploads, in slots (map 0's, then map 1's over them: "P2, revised" in `PC_RENDERER.md`). They land
   at pages 350 and above, 32-bit, 8-bit and 4-bit.
4. PATH1: the scrolling textures drawn into their 128 x 128 targets (pages 450, 466, 468) and, at 009, the local copies
   that scroll them; at 008, 009 and 014 the reflection's view into its 256 x 256 16-bit target (page 350) with its Z
   in the frame's Z buffer, then `GsDraw_ClearDepth`.
5. The world as native draws: the map's records and objects, the shadows (after `GsDraw_SetScreenAlpha(0)`: DATE),
   the entities, items, effects, late records, then the eye's camera (mode 2): the arm and what is held. Each
   `RD_ND_DRAW` is a draw of the side buffer: its kind and identity, program, camera, matrices, lights, fog, point
   lights, meshes (`render.h`'s `RdDraw`); its material is the chain's registers and memory at its place.
6. The passes after the world (`RD_KIND_PASS`): the tint, the wobble (with the frame's copy to 0x2BC0 before it),
   fills; the HUD as GIF; the fade; the brightness last.
7. The vblank: the display registers name the buffer shown.

No read-backs in play (two in the whole pond run, from uploads).

### The scene

A **scene** is one game frame, built as the renderer's thread meets the chain (it must be: a draw's material is the
store's state at its place), then immutable and shared with whatever draws it (the frame's own picture and every
picture between of it):

```cpp
// One game frame as a scene (port/gs/scene/, the slices below). Built on the renderer's thread in the chain's
// order; read-only once the frame's vblank closes it; held by every picture drawn from it.
struct Scene {
    uint32_t serial;                         // RD_ND_FRAME's
    std::vector<Pass> passes;                // in the stream's order: a pass is a run of draws into one target
    std::vector<RdCamera> cameras;           // the frame's (rd_camera's indices)
    std::vector<Instance> instances;         // a native draw's record (vu1_record.hpp's) and its identity and move keys
    std::vector<Material> materials;         // resolved: pipeline key, bindless texture, CLUT, sampler, GS constants
    std::vector<Draw> draws;                 // in order; a draw names a pass, a material, an instance or 2D items
    std::vector<Item2d> items;               // the GIF primitives left (the HUD, clears as clears, sprites), with
                                             // their kicks' moves (gif_moves.h) for pictures between
    std::vector<Layer> layers;               // the full-screen passes folded into the final pass, in order
    std::vector<Op> ops;                     // copies between targets, the frame's copy to 0x2BC0, DATE resolves
    FrameTargets shown;                      // which buffer is drawn and which is shown
};
struct Pass {
    TargetId colour, depth;                  // images of their own ("3. The targets")
    LoadOp colour_load, depth_load;          // a clear the pass starts with (the swap's, ClearDepth), or keep
    uint32_t first_draw, draw_count;
    CameraUse camera;                        // the world, the mirror, the reflection's, the eye's
};
struct Draw {
    uint32_t material, instance;             // the instance: no_instance for 2D
    uint32_t mesh_first, mesh_count;         // triangles in the arena (persistent meshes), or items' range
    uint32_t kind, id[3];                    // RdDraw's identity: what pictures between pair it by
    uint16_t flags;                          // fused sphere pass, DATE id, far Z ...
};
```

- **Native draws** become draws directly: the meshes stay in today's arena (placed once, persistent), the instance
  is the record `vu1_record.cpp` makes (its maths on the GPU: `vu1_*_maths.h`), the material is resolved from the
  registers the batch's A+D packet leaves (written through the GIF front end as today, so the GS's state after the
  draw is the PS2 path's).
- **GIF primitives** become 2D items: clears are recognised as clears (a pass's load op), the passes that `pass_draw`
  makes are layers (folded or drawn), the rest are items drawn in order with the GS's pixel arithmetic.
- **Uploads and copies** go to the texture store (which keeps answering materials) and to the targets they fall on.
- **Where it is built**: the renderer's thread, as today's `Intake` meets the chain (`pcr.cpp`): the GIF front end,
  the texture store and the native draws' records are reused as they are. What changes is that it records nothing:
  it builds the scene and hands it on at the vblank ("8. Scheduling and present").

## 2. Materials and the GS's memory

A draw's material is the GS's registers (TEX0, TEX1, CLAMP, TEXA, TEST, ALPHA, FBA, FOGCOL, the context's) and its
memory at the draw's place in the chain. Today `pcr_textures.h`'s store resolves it: every upload kept as a chunk found
again by its bytes, a page-ownership table, a texture decoded once from the uploads that own it, the CLUT mirrored,
and a kept answer for a question asked again while its pages are unchanged. The census says why that is the right
resolver to keep:

- **A frame's uploads are nearly all the same bytes again**: 2.6-7.7 MB a frame (the game re-sends each slot's
  textures every frame, map 0's then map 1's over them), and at the pond over the whole run 3.24 GB uploaded into 736
  distinct chunks; 180,310 resolves, 345 decodes, 176,975 found again, 169,035 answered from a kept answer. Content
  is what identifies a texture; the store already keys by content.
- **Order matters and only the chain knows it**: map 1's uploads go over map 0's pages after map 0's draws. Resolved
  in the chain's order, each draw's material is right; resolved at the end of the frame, map 0's draws would read map
  1's texels ("P2, revised").
- **Textures drawn by the frame** (the scrolling targets, the reflection, the frame's copy) are not uploads: the store
  answers `DrawnSurface` and the draw reads the target's image (below).

**Recommendation**: keep the store as the resolver, on the renderer's thread in the chain's order; a resolved texture
becomes an entry of a bindless table (RGBA8 with its levels, as the GPU's sampler reads it today: `colour_texels`),
kept by its `TextureKey` (content: the chunks, shape and the part read) and the CLUT's colours; the material holds
the table index, the sampler and the GS constants (TFX, TCC, AREF, FIX, FOGCOL ...). The scene holds its materials'
entries (reference counts), so a texture the store lets go stays alive while a scene that drew it lives.

**What this means for pictures between**: a picture between draws the frame's scene with the frame's materials,
whatever has been uploaded since. Today's replays ask for the frame's uploads again (0x7C) and walk the store
(`KeptWalk`, `fits`, "the store changed"); the scene needs none of it. The only reads of memory a picture between
makes are of targets the frame drew (below), which it either reuses (they do not move with the camera) or draws again
(they do).

**CLUTs**: kept as today (the mirror, CLD loads at TEX0 writes), the colours expanded into each texture's RGBA8 at
decode (`colour_texels`) as today's sampled path does; a texture with another CLUT is another entry (the census: 26-40
textures a frame, so the table is small).

## 3. The targets

From the census and the inventory's "Targets and the GS memory they share":

| target | GS memory | in a frame | read by | the scene's |
|---|---|---|---|---|
| the frame buffers | FBP 0 and 140, 640 x 448 32-bit | every frame | the scanout, the passes reading the frame (folded), the frame before blended in (the history), the copies | two RGBA8 images (or one and the history: below) |
| the frame's Z | ZBP 280, PSMZ16S | every frame | the world's tests | `D16_UNORM`, cleared by the pass's load |
| the scrolling textures' targets | FBP 450, 466, 468 (128 x 128, 128 x 160), their Z over page 280 at 128 wide | every map | the sky, water, lava, the flesh cavern (as textures) | RGBA8 images of their own; their Z an image of its own (`clear_depth_aliases` today) |
| the scrolled textures | pages 459-482 (local copies, the pond: 2 a frame) | where they scroll | the same draws | copies between images, made in order |
| the reflection's view | FBP 350, 256 x 256 PSMCT16S, Z in the frame's Z at 256 wide | 008, 009, 014 | the ripple water (as a texture) | an RGBA image of its own (5:5:5:1 kept by the shader's arithmetic), its own D16 |
| the frame's copy | 0x2BC0 (pages 350-489), 640 x 448 | under water, the MP drain, the map change's fade, the pause and map viewers | the wobble, the fades | an image copied from the frame at its place in the order (with its alpha) |
| the Dark Succubus' copy | 0x3540, 256 x 256 | 038 | her model | an image drawn from the frame at its place |
| the menus' grab | 0x3240 | when a menu opens | the menu's backdrop | an image kept by the menu loop |
| the menus' field buffers and 32-bit Z | FBP 0 and 70, ZBP 140 | the menu loops | the scanout | images of their own, D32F |

- **Images of their own** for every target (`world-plain.md`: no extra cost; fast clears need it). A target is found
  by its GS identity (base page, width, format) as the canvas's surfaces are today; what aliases matters only where a
  target is read: the frame's copy over the reflection's view, the scrolling targets inside the copy's range, the
  menus' field buffers over the frame buffers. Those are written in order (a copy into the copy's image, the
  reflection into its own), and a texture read of their pages asks the store, which answers which was written last
  (the page-ownership table already knows "drawn into or copied over since"). Measured nowhere in play: a read of a
  target whose pages another target wrote after it (the inventory found none but the copy's range, which the game
  uploads again after a copy: `Effect_UploadTextures`).
- **Clears**: the swap's first clear is dead (the second clears the same buffer the same way): one load op. The depth
  clear (`GsDraw_ClearDepth`) is the next pass's depth load op or a depth-only clear. The reflection's
  `GsDraw_ClearFrame` its pass's load op.
- **The frame's Z aliases** (the scrolling targets' and the reflection's Z over page 280 at other widths): each an
  image of its own, cleared as its pass begins. The game never reads one through the other (it clears them before
  use: the inventory's open question 1 is the canvas's bookkeeping, not the game's).
- **Read-backs**: none in play; the store answers the two at load from uploads.
- **The history**: the frame before blended in (`Game_BlendPreviousFrame`, the knock-back) reads the last picture
  shown; the final pass keeps that picture as an image (it is the presented picture before scaling: below).

## 4. Depth's exactness and DATE

### The two candidates, measured

The tour (64 maps, 760 views, 1x, native draw on, 30) and the motion cases (13 cases, 745 pictures, 1x), against
paraLLEl-GS, pixels differing by more than 24 levels; and the world's GPU time at the owner's 2x on the Vega 7 (a
real window, the owner's settings, medians of two rounds):

| depth | tour | motion | world at 099 / pond / palace (ms a game frame) | frame at 099 / pond / palace |
|---|---|---|---|---|
| today's rule (interlock, exact Z's planes) | 8,845 | 1,126,375 | 5.37 / 10.30 / 9.31 (`depth-by-layers.md`) | 15.66 / 20.31 / 19.31 |
| **`D16_UNORM`, Z clamped at each pixel** | **7,008** | **1,120,237** | **2.85 / 6.11 / 6.22** | 12.64 / 16.29 / 14.80 |
| the GS's whole Z in the shader (`gsexact`) | 9,371 | 1,125,827 | 4.25 / 9.84 / 8.50 | 13.62 / 19.01 / 17.24 |
| the GPU's depth in float, Z clamped at the corners | 16,740 | | 2.85 / 6.00 / 6.24 | 11.81 / 14.40 / 14.55 |

Against today's rule, view by view: UNORM moves 1,813 pixels nearer paraLLEl-GS and 25 further; gsexact 730 nearer
and 1,262 further (motion: UNORM 6,549 nearer and 494 further; gsexact 3,347 and 2,947).

**How many of UNORM's 7,008 pixels are depth at all.** For each pixel UNORM draws more than 24 levels from
paraLLEl-GS, whether any other depth mode draws it within 24: today's rule fixes 12, gsexact 11, the GPU's float depth
27, the float depth clamped at each pixel 25; **any of them, 33**. The other 6,975 are drawn the same wrong way by every
depth mode: 5,549 at 099 (view 6: the one view over 0.4% in every mode, not depth), 473 at 002, 465 at 027, 107 at
098, 79 at 013. So, as far as the four depth modes tried can tell, the depth part of "exact" is a few dozen pixels in
760 views. That is a bound from what was tried, not a proof: a depth stepped exactly as paraLLEl-GS steps it (not
built) could fix pixels none of these fixes. But the 6,975 are drawn the same wrong way by every mode, and 5,549 of
them are one view known not to be depth, so little is left for any depth to fix.

**Why the GS's arithmetic in the shader is further, not nearer.** It is the PC renderer's exact Z (the plane through
the corners worked out in integers, rounded half to even at the pixel's point), not the GS's rasterizer: paraLLEl-GS
steps Z across the triangle as the GS does, and where the two round differently a tie becomes a win. The crops
(`compare/crops/gsexact_*.png`): map 019's door hinge cut into the wall where the wall is drawn after it on the same
whole Z (73 pixels; UNORM and the rule draw it as paraLLEl-GS does), map 100's wall edge (139), map 112 (134). The
GPU's D16 rounding of its interpolated depth happens to land with paraLLEl-GS's far more often. Matching paraLLEl-GS's
own Z stepping in a shader would be a third design (its DDA per pixel, written as the depth: the cost of gsexact or
more) for what the bound above says is a few dozen pixels.

**Cost**: gsexact needs the planes (a compute pass of 64-bit integer maths over every native triangle, 0.7-1.4 ms a
game frame on the Vega 7: `world-plain.md`) and a depth written by the shader (early tests on the corners' bound
only); UNORM needs nothing but the depth unit.

**Decided (the owner's decision 1, 2026-11-19)**: `D16_UNORM` for the game's 16-bit Z buffers (every 3D draw: PSMZ16S),
the depth Z / 65535 clamped at each pixel by the depth clamp to the viewport's 0 to 1, the GS's test as the GPU's
(GEQUAL, GREATER, ALWAYS, NEVER), invariant positions; D32F for the 24- and 32-bit Z buffers (the menus'). "Exact" for
depth then means: the GS's whole Z, rounded by the GPU, ties decided by the later draw as on the GS; single pixels
along steep edges differ where the two roundings fall either side of a half (25 pixels against today's rule in the
tour; at most 33 that any depth would fix).

### DATE

DATE is two things in play (the census, every scene):

1. **The shadows** (`Entity_DrawPoolModels`, program 0x5A0, kind `RD_KIND_SHADOW`): 24-164 draws a frame, after
   `GsDraw_SetScreenAlpha(0)` set every pixel's alpha to 0. TEST_2 0x55001: the alpha test NEVER with AFAIL FB_ONLY
   (the frame written, **Z never written**), DATE with DATM 0 (drawn where the alpha's bit 7 is 0), Z GEQUAL; FBA_2 1
   (each pixel drawn sets bit 7); ALPHA_2 FIX (the entity's alpha / 4), the colour FOGCOL (0x5A0's colour and fog 0).
   So **each pixel is darkened once, by the first shadow drawn there that passes the depth test**. Their fragments:
   0 at 099's start and the pond's moment, 1,854 under water, 12,159 at the palace, 28,824 in combat (at 2x: under 2%
   of a screen).
2. **One HUD sprite** a frame (`p1 sprite tex blend date`, 4,920 GS pixels), which tests the alpha the world wrote since
   the reset.

`D16_UNORM` has no stencil (`D16_UNORM_S8_UINT` is optional, and not on every GPU). Options:

| option | exact? | cost | needs |
|---|---|---|---|
| a copy of the target per DATE draw (the experiment's) | yes | 0.3-1.9 ms a game frame at 2x | nothing |
| the target's alpha read in the shader (`VK_EXT_attachment_feedback_loop_layout`) | no: a shadow's triangles overlap each other within a draw, and the read is unordered within a draw | small | the extension (not everywhere) |
| a stencil image of its own (D32F_S8 for the whole frame) | yes | D32F's depth is not the GS's 16-bit rounding: loses UNORM | |
| **the first draw's number, order-free** | yes | the shadows' fragments and one pass over their rectangle | MIN blend on an R16_SFLOAT image (core) |

**Recommendation**: the shadows as a **first-draw image**. Each shadow draw is drawn (Z GEQUAL against the frame's
depth, no Z write, as the GS) into an R16_SFLOAT image (the frame's size, or the shadows' rectangle; whole numbers to 2,048 exact, the most shadow
batches counted a frame 352) with a MIN blend, its output its number in the frame's order; nothing else is drawn there. One pass over the image's written rectangle then
darkens each written pixel as the first shadow there would have, ((FOGCOL - Cd) * FIX >> 7) + Cd with the GS's
arithmetic, from a table of the draws' FIX and colour, and writes the alpha 0x80 (FBA). That is exactly the GS's
result, since no shadow writes Z and the first one to pass the depth test is the one whose colour stays. It needs
`GsDraw_SetScreenAlpha(0)` before the shadows with nothing between them (`Entity_DrawPoolModels` sets the GS up
itself around the loop): the scene checks it, and a frame where it does not hold draws DATE by a copy of the shadows'
rectangle.

**The HUD's DATE sprite**: drawn with the frame's alpha read from a copy of its own rectangle (4,920 GS pixels, 20,000
at 2x: nothing), the shader testing bit 7 as the GS's DATE.

**The alpha's reset** (`GsDraw_SetScreenAlpha(0)`, 0.42 ms a game frame at 2x: `fullscreen-layers-inventory.md`):
nothing reads the frame's alpha between the swap's clear and the reset (no DATE, no blend by the destination's alpha,
no copy of the frame: to be checked by the census in slice 1 on every scenario, a frame where it fails drawn as now),
so the frame's clear writes alpha 0 instead of 0x80 and the draws before the reset write no alpha (their colour write
mask RGB). The alpha is then 0 everywhere at the reset, as the GS has it; the reset costs nothing.

## 5. Draws: order, batching, the skinned models

**The order is the stream's** (settled). What the census says of how few calls that can be:

| | 099 | pond | palace | combat | under water |
|---|---|---|---|---|---|
| draws in order | 1,208 | 3,286 | 4,474 | 1,895 | 860 |
| runs of one pipeline (textures bindless) | 207 | 1,020 | 1,523 | 301 | 188 |
| of them program 0x280's | 139 | 927 | 1,437 | 247 | 146 |
| runs of one pipeline and texture (no bindless) | 225 | 1,044 | 1,655 | 317 | 202 |
| today's native draw calls | 263 | 1,313 | 1,826 | 372 | 206 |

- **Program 0x000** (the map, objects, items, shadows): 522-2,036 batches in 22-65 pipeline changes. Each run of one
  pipeline is one `vkCmdDrawIndirect` of its batches (a command each, the record in the first instance, as today's
  kept walks' `Runs` already draw them) with textures from the bindless table: **20-70 calls a frame**.
- **Program 0x280** (entities, the map's skinned objects, the arm, gold): its batches alternate with their **sphere
  passes**: every batch with a second pass changes TEST, ALPHA, texture and PRIM for the pass and back (the census:
  of the palace's 2,006 consecutive batches, 1,435 change the blend and the texture; only 78 change the record). That
  alternation is nearly all the runs left: 927 of the pond's 1,020, 1,437 of the palace's 1,523.
  - **A batch's two passes as one draw** (recommended, with a check): the sphere pass is the same triangles (the same
    XYZF2s), its Z the first pass's, its test GEQUAL; where the first pass is unblended or its blend leaves the
    source alone (`alpha_is_one`), the first pass's colour is known in the shader, and the sphere pass's blend over it
    can be made there with the GS's integer arithmetic, written once. Exact but for triangles of one batch that
    overlap on the same whole Z (the GS draws the second's first pass, then both sphere passes over it; one draw
    draws each triangle's two passes together). How often that changes a pixel is measured in slice 3 against the
    two passes drawn apart; where the first pass is blended by its own alpha, the two passes stay two draws.
  - Then the runs left are the 0x280 draws' own state changes (the census's "record" and pipeline: tens a frame).
- **Program 0x700** (effects): 2-110 batches in 1-20 runs.
- **What varies within a run** is the record (the instance), the texture (bindless) and the triangles' range: an
  indirect command's first instance names the record, its push or a per-draw word the texture.

**So a frame is an estimated 100-250 calls** (the runs at the pond and the palace less 0x280's alternation, its own
changes of record and state, the 2D and the passes; slice 3 counts them), against 263-1,826 today; the calls' CPU cost is then small next to the triangles (12,000-44,000 a
frame).

**The VU1 maths stay on the GPU** (`vu1_*_maths.h` in the vertex shader, the records from `vu1_record.cpp`), and each
corner's Z comes from them as the GS gets it (the corners' XYZ after FTOI4, which is what `D16_UNORM` needs: the GS's
Z at the corners, interpolated and rounded by the GPU). The CPU's part stays: VU1's clipping of each 0x280 and 0x700
batch's kicks (`vu1_kicks_280`, `vu1_kicks_700`: which triangles a run draws) and the triangle lists they give.

## 6. The 2D and the passes

- **The HUD, the menus, the text, the movies**: GIF primitives drawn as today's canvas draws them (the GS's pixel
  arithmetic in `pcr_prim.frag`, Vulkan's blend where it has the form: `pcr_blend.h`), as the scene's 2D items in
  order, batched by state. They are 4-460 primitives a frame; nothing gains from more.
- **The clears** are load ops (above); **the dead writes** (the swap's first clear, `GsDraw_ClearDepth` where nothing
  tests Z after it) left out by `pcr_dead_writes.h`'s rules run on the scene.
- **The screen passes fold into the final pass** where the inventory says they may (its classes; the blockers kept):
  - the brightness (`RD_PASS_SCALE` at the brightness site), the fills and fades after the HUD (MapDrawState's fill,
    `Game_DrawFade`, the message dim, the pause's shade): a list of layers, each ((A - B) * C >> 7) + D a channel with
    COLCLAMP, applied in order to each pixel in the final pass, before scaling (each a byte, as the GS's);
  - the tints and fills **under the HUD** (the ailments, the damage flash, the death fade, the green fill at 035) and
    `Game_TintScreen` under water: drawn at their place, as now (the HUD is drawn over them), or folded with the HUD
    kept out (the HUD then drawn into an image of its own and composed in the final pass: a later step if the cost
    says so: they are 0.5-1.3 ms only where they show);
  - the frame before blended in (the knock-back, bit 40): reads the **history**, the last picture before scaling,
    which the final pass keeps; folded as a layer reading it;
  - the wobble (under water, the MP drain): the frame's copy to 0x2BC0 made at its place (an image copy), then the
    wobble's 140 triangles drawn from it as now; folding it would sample the copy with the moved coordinates in the
    final pass (a later step);
  - the flicker filter and the view-turn shift (off by default on the PC), haze and the depth sprite (unreachable):
    drawn as now.
- **The alpha copied to 0x2BC0** (under water): the copy is an image copy of the frame with its alpha, made at its
  place; nothing folds across it.

## 7. Pictures between

A picture between at time t is **the frame's scene drawn again** with:

- the camera t of the way (`frame_interp.c`'s slerp and line, `interp_camera.h`'s cuts), and mouse look's lead;
- each instance moved as `part_moves` moves it today (its record's `VU1_MOVE_*`: with the camera, between its two
  places, by its matrices; the arm by the eye's), the shaders' `moved()` as today;
- the 2D items moved by their kicks' moves (`gif_moves.h`: FLAT moves t of the way, pairs by the frame before's), the
  passes by `PassMoves` (the fills' colour, the wobble's cells, the frame before by A^t);
- the trails' points t of the way from their partners (on the GPU, as `draw_gs_strip`'s moves);
- **the targets that move with the camera drawn again** (the frame, the reflection's view, the frame's copy and the
  Succubus' copy at their places), **those that do not reused from the frame** (the scrolling textures' targets: drawn
  from the frame's textures at the game's rate; today's replays draw them again to the same pixels).

What today's kept walks refuse (`FrameWalkRefused`) is the stream's way of drawing that the scene does not have:
`reads_drawn` (a pass reading the frame: a folded layer, or a copy op of the picture's own target), `copies_drawn` (the
frame copied to 0x2BC0: the copy op made from the picture's target), `uploads_drawn` (an upload into the frame: the
movies, which have no pictures between), `trail_paired` (the trail on the GPU), `native_unpaired` and `marks` (the
stream's bookkeeping). None of them stops a scene.

What it needs of the frame's targets: the scrolling targets' images (held by the scene) and the history; **none of the
pump's surface copies** (2.4-3 ms a picture between today).

**Instant mouse look.** Today a vblank's own picture is drawn again whole, turned to the newest mouse reading
(`camera-whip-slowdown.md`, problem 2), and the lead grows with how late the game spends the hand's turn: about two
game frames and two vblanks (`mouse_lead.h`), so a fast whip of 6 rad/s leads by up to about 0.4 rad (23 degrees), a
flick of 20 rad/s by more than a radian. A reprojection of the finished picture with a margin cannot cover that: the
margin would be most of the screen. **Recommendation**: mouse look is a picture drawn from the scene with the newest
camera, which once a picture between costs a fraction of a game frame is the cheap way; the reprojection is kept for
the last turn only (`pcr_late.h`'s `TurnLead`: the hand read a few milliseconds before the picture is shown, a turn
of a degree or two), drawn with a margin of the scene a little past the screen (a wider viewport, a few percent more
pixels; slice 4 sizes it from the late turns measured) so that its edges are never empty.
Parallax: none for a turn about the eye, which mouse look is; the camera's position moves only between game frames.

## 8. Scheduling and present

**Why today's cut left the game at 21 with no pictures between** (`camera-whip-slowdown.md`, problem 1). Run again
here (the owner's settings in a real window, `KF4_DIAG_SPIN=6`, `--stats`; `whip/` in the measurements): the game at
18.4 frames a second in the last half second, 18.4 pictures shown a second, and:

- **the cut never fired**: no picture between was left out as behind and the cut was never lowered (`--stats` prints
  neither). `gs.c` lowers it when `gsr_behind()`, which is `pcr_backlog() > 2`; but every vblank's picture holds the
  sender in `Inbox::wait_behind_at_most(2)` until the renderer is at most two vblanks behind, so by the time `gs.c`
  looks, the backlog is never more than 2. The back-pressure drains the very backlog the cut waits for, and holds the
  game instead;
- **the pictures between were all drawn and then not shown**: 1,182 drawn for 820 game frames, and the window left 374
  pictures out as late ("a newer one due"). The GPU paid for each (25.5 ms of GPU a picture in the profiled window: the
  2D 8.7 of it, the world 9.1), so the window's counter showed no pictures between while the GPU drew them;
- and each game frame was drawn again for the exact restore (819 of 820 frames "drawn again for buffers they
  changed": `port_interp_end_vblank`, the scrolling textures' targets), on top of mouse look's redraws of the vblank's
  own picture.

So the cut judged the wrong thing (a queue the back-pressure keeps short) and acted on nothing the GPU had already been
given. The scene renderer's rule is the other way round: decide before a picture is drawn, from its predicted cost,
and never hold the game.

**The scene renderer's rules**:

- **The game is never held by pictures.** The renderer's thread builds the scene and hands it on; the only wait the
  game's side ever has is for the scene slot two frames back (the side buffer's ring, as now). A game frame's own
  picture is drawn first, always.
- **Pictures between by budget**: each picture's GPU cost predicted from the last few of its kind (the GPU's
  timestamps, `pcr_gpu_time`'s, by pass: the world, the reflection, the 2D, the final pass), and a picture between
  asked for only when its predicted cost fits before its time with the frame's picture ahead of it; one that does not
  fit is left out, never waited for. At most one picture in flight beyond the one on the screen.
- **Threads**: the renderer's thread (today's kf4-pc) builds scenes from the chain; a drawing thread records and
  submits every picture (the frame's and its pictures between) from the newest scene, and the present. No canvas on
  two threads, no surface copies between them: the scene is shared read-only.
- **The final picture** is one pass a picture: the shown target read at the picture's scale, the folded layers applied
  in order (bytes, as the GS's), the history kept (the picture before scaling, for the frame before blended in), then
  scaling with sharpening and FXAA or SMAA (`pcr_antialias.cpp`'s passes; about 2.7 ms a vblank at 2x today, of which
  separate passes are a part to measure in slice 5).
- **A vblank that shows the same picture** (at 30, the second vblank of each game frame; a menu loop with nothing
  new) presents the image already made (a copy to the swapchain's image), not a new one.

## 9. Beside today's renderer, and how it is judged

- **`--renderer scene`** beside `pc` and `gs` (`port/platform/gs_api.h`, `gs.c`): its own folder (`port/gs/scene/`),
  the same GS API (packets, vblanks, pictures between, the presenter's callbacks and scanout ring, save states,
  captures), the same settings, overlay and presenter. Developers only until it replaces `pc`.
- **What it reuses**: the GIF front end (`pcr_gif.*`), the texture store (`pcr_textures.*`), the native draws' interface
  (`render.h`, `rd_fetch`, the arena's meshes), `vu1_record.*` and `vu1_*_maths.h`, `part_moves.*`, `pass_draw.*` (as
  data: `PassMoves`, the passes' arithmetic), `gif_moves.h`, `ripple.*`, `pcr_blend.h`, `pcr_prim.frag`'s pixel
  arithmetic for 2D, `pcr_dead_writes.h`'s rules, `pcr_gpu_time.*`, `pcr_hang_watch.*`, the pipelines' cache
  (`pcr_pipelines.*`), the anti-aliasing and present passes (`pcr_antialias.*`, `pcr_present_*`), the presenter.
- **What it needs of the device**: Vulkan 1.1 with `descriptorIndexing`'s sampled-image array, non-uniform indexing,
  partially bound and runtime arrays (the bindless table); `multiDrawIndirect` and `drawIndirectFirstInstance`;
  `depthClamp`; `D16_UNORM` as a depth attachment (required of every Vulkan device); blending into R16_SFLOAT (required);
  `independentBlend` only if the HUD's own image is added. **Not**: fragment shader interlock, 64-bit integers, dynamic
  rendering, a stencil with the depth. The integrated GPUs of the last ten years are listed with these by their
  drivers (Intel Gen9 and later, AMD GCN and later; NVIDIA's from Kepler); checked here on the Vega 7 only. **To check on Intel and NVIDIA** (the
  owner's or a contributor's machine): the tour with UNORM, since how a GPU rounds its interpolated depth into D16, and
  clamps it, is its own within Vulkan's precision rules; RADV's is what was measured.
- **What it needs of Granite**: its bindless pool (`BindlessDescriptorPool`), indirect draws, its render passes and
  images; the edits `cmake/granite_*.cmake` already make (dynamic state, depth bounds, pipeline statistics).
- **How its pictures are judged**: the tours and motion cases against paraLLEl-GS (`tour_compare.py`,
  `motion_compare.py`), no further than today's renderer in total and per case; byte for byte against itself where a
  slice claims exactness (folding, fusing, batching); the GPU's time in a real window with the owner's settings
  (`world_plain.py` and `layer_inventory.py --steps live` taught a `scene` renderer), on the Vega 7 and the 9070 XT.
- **Captures already hold the native draws** (`render.h`, "Captures": `KF4GS_ND_MESH` and `KF4GS_ND_DRAWS` beside the
  chain; `kf4-gsreplay` fed a capture of 099 back as 652 draws and 1,057 meshes). So a scene can be replayed without
  playing: `kf4-gsreplay --renderer scene` is slice 1's.

## 10. The slices

Each is a renderer someone can run (`--renderer scene`), with what it must show to pass. The GPU ms are the Vega 7's at
the owner's 2x in a real window.

| slice | what | passes when | risks |
|---|---|---|---|
| 1. Game frames at 30 | the scene built from the chain; the world in order, unbatched (a draw a native batch); GIF 2D; targets as images; `D16_UNORM`; DATE by the first-draw image; the passes drawn as GIF (not folded); the unidentified 0x000 GIF 3D found | tour against paraLLEl-GS within UNORM's 7,008 (no worse than the rule's 8,845); every motion case at 30 no further than the rule; the layer inventory's every scenario draws; the world's GPU ms within 10% of `unorm`'s (2.8 / 6.0 / 6.1) | targets aliasing in ways the census did not reach (the map change, the map viewers, the menus' grab); the menus' 32-bit Z |
| 2. The frame's layers | clears as load ops, dead writes left out, the alpha's reset folded into the clear, the passes after the HUD folded into the final pass, the history image | pictures byte for byte slice 1's; a game frame 2.5-3 ms less at 2x (the inventory's 0.47 + 0.66 + 1.75 + 0.42) | a reader of the alpha before the reset somewhere (falls back per frame) |
| 3. Batching | bindless textures; runs as indirect draws; 0x280's two passes as one draw where exact | pictures byte for byte slice 2's where claimed exact, the fused passes' pixel changes counted and shown to the owner; 100-250 calls a frame; the world at 099 under 2 ms (after 3b: not batching's to give, judged by part 3f's study of where the GPU's time goes), the pond and palace's 0x280 under 1 ms | the fused passes' overlap; drivers' indirect draw speed on integrated GPUs; indirect draws on RADV (the PC renderer's hang on the 9070 XT) |
| 4. Pictures between | the scene drawn at t; mouse look from the scene with the late turn's reprojection | `motion_compare.py` at 60 and 240 no further from paraLLEl-GS than today's pictures between; a picture between under half a game frame's GPU time | parts paired by identity that today's replay pairs otherwise (look-alikes) |
| 5. Scheduling and present | the budget, the threads, one final pass, a repeated vblank not made again | the mouse spin at the owner's settings holds 30 game frames a second, with as many pictures between as fit; a repeated vblank under 0.3 ms | the GPU's timestamps' spread on a busy integrated GPU |
| 6. For players | settings, the overlay, save states, `pace_test.py`, `map_tour.py`; the 9070 XT and an Intel or NVIDIA GPU | the owner's review; then `pc` retired | |

Slice 1 is built and measured (2026-11-19): `scene-slice1-results.md`, each criterion with its numbers. It passes but
for DATE's 0.1 ms (0.39 at the pond, against the PC renderer's 2.5 there with its copies: the owner accepts it for now,
to come back to once more of the renderer is done) and the copies of the reflection and the HUD's DATE sprite.

Slice 2 is built and measured (2026-11-20): `scene-slice2-results.md`. Clears held back into load ops (dead ones never
drawn), the alpha's reset folded into the frame's clear, the brightness drawn by each picture in place of its copy out,
the history made where a texture reads such a frame; every picture byte for byte slice 1's. A game frame is 0.9 / 1.7 /
1.4 ms less at 099 / the pond / the palace, short of the 2.5-3 asked: the fills after the HUD are not folded (byte for
byte they must be the GPU's blend, drawn again each picture), the world's time grows 0.4 ms with the alpha's fold (not
yet explained), and at the pond the frame's clears are partial render areas. After it (`scene-slice2-after.md`): the
0.4 ms was the shadows' first-draw image's clear, timed in the world's last draw (the image is now cleared once in 128
frames), and the fills after the HUD are folded byte for byte; a game frame 0.35 / 0.6 / 0.17 ms less again.

Slice 3 is cut into five parts, a session or so each (2026-11-20, `HANDOFF_scene_slice3.md`): 3a records and the
bindless table, still a call a draw, with the counts and the baseline (the detail's timestamps off too); 3b runs as
indirect draws; 3c the level off paraLLEl-GS at 1x found or bounded (a study, any time before 3d is judged); 3d 0x280's
two passes as one draw behind a switch, off until the owner has looked at its changed pixels; 3e the slice judged
whole and the owner's decision on 3d. Their numbers go into `scene-slice3-results.md` as each is done.
3a is done (2026-11-20): every draw from the scene's records with its textures bindless, the pictures byte for byte
slice 2's; the runs counted (243 / 1,053 / 1,652 a frame at 099 / the pond / the palace, 0x280's 139 / 885 / 1,437, as
the census); the GPU time slice 2's at 099 and the pond, 0.12 ms more at the palace (0x280's many small draws). The
fragment stage reading its record in an indirect draw is 3b's first question.
3b is done (2026-11-20): runs as indirect draws, the fragment stage shading a record at a time (one flat value from the
vertex stage), unblended batches drawn in runs blended by As where exact; pictures byte for byte 3a's; calls a frame
171 / 971 / 1,478 (0x000's 13 / 55 / 25). The GPU time 3a's at 099 and the pond, 0.15 ms less at the palace; 0x000 at
099 still 2.6 ms: its calls were not its cost. Batching saves the CPU's time, which the GPU's timestamps do not see. A
sixth part, 3f, studies where the GPU's time goes (0x000's pixels, the 5.5 ms scanout) and the calls' CPU time; the
"under 2 ms" is judged by it (`HANDOFF_scene_slice3.md`, "After 3b").

Slice 4 is built and measured (2026-11-20, `scene-slice4-results.md`): pictures between from the kept scene into
stand-ins (4a, no exact restore); mouse look from the scene, the frame's own passes held back where every picture of it
is a picture between (4b: the third world draw a game frame at 60 gone, byte for byte). The late turn measured is not
"a degree or two": 12 ms at 60, 5.3 degrees at 6 rad/s, mostly the picture drawn a vblank before it is shown. Decision
3's reprojection is therefore moved after slice 5 (pictures begun just in time), and needs the HUD in an image of its
own (decision 4) not to move the HUD with the world.

Slice 5 is built and measured (2026-11-21, `scene-slice5-results.md`). Pictures are begun just in time from their
measured cost (`pcr_budget.h`: by kind, the vblank's scene timed as GPU work of its own). A picture between that would
not be ready by its time, or that would make a game frame's own picture late, is left out, and gs.c's cut is off under
it. A vblank with nothing new is its last picture published again. The spin holds 30 game frames a second at 60, 240 and
unlocked, every picture on time after the first two seconds. The late turn at 60 is 6.6 ms (was 12.2), the picture's own
cost. A repeated vblank costs the renderer no GPU work. The design's drawing thread is not built: kf4-pc waits for each
picture's time between building scenes, enough at 60, but at 240 one picture in seven or so is begun late. The
presenter's copy into the swapchain is not yet in the final pass.

**When today's renderer can go**: after slice 6, when the scene renderer is no further from paraLLEl-GS than today's
in the tours and motion cases, draws every scenario of the layer inventory, and the owner has played it.

## The owner's decisions

2026-11-19: the owner took the recommendations, the well-supported ones as settled, the others as the direction to
build and measure, each to be looked at again only on its slice's numbers.

1. **Depth: `D16_UNORM`. Settled.** UNORM is 7,008 pixels from paraLLEl-GS in the tour (gsexact 9,371, today 8,845),
   1,120,237 in motion (gsexact 1,125,827, today 1,126,375); of its 7,008, at most 33 are fixed by any depth mode
   tried; gsexact's world costs 1.4-3.7 ms more a game frame at 2x (1.40 at 099, 3.73 at the pond, 2.28 at the palace)
   and needs 64-bit integers. "Exact" for depth means the GS's whole Z rounded by the GPU, with single pixels along
   steep edges either side of a half. D32F for the 24- and 32-bit Z buffers (the menus'). Still to check before
   players get it: the tour on an Intel or NVIDIA GPU (only RADV's rounding was measured).
2. **Program 0x280's two passes as one draw, where the first pass is unblended (or its blend leaves the source alone).
   The direction; slice 3 builds and measures it.** Exact but for triangles of one batch on the same whole Z, whose
   sphere pass the GS draws twice over the later one. Slice 3 counts how many batches it applies to (not counted yet:
   the world is mostly drawn with a blend by As), the pixels it changes against the two passes drawn apart, and the
   calls a frame left; the owner looks at the changed pixels before it is kept. Where it does not apply the two passes
   stay two draws.
3. **Mouse look drawn from the scene with the newest camera, the last few milliseconds' turn by reprojection with a
   small drawn margin. The direction; slice 4 sizes it.** The lead of about 0.4 rad at 6 rad/s is read from
   `mouse_lead.h`, not measured; slice 4 measures the late turns and sizes the margin from them.
4. **Tints under the HUD drawn at their place. Settled for now** (0.5-1.3 ms only where they show); the HUD in an
   image of its own, so that they fold too, only if a later measurement says it is worth it.

5. **The fills after the HUD folded with the GS's arithmetic. Decided (2026-11-20).** The fade, the dim, the pause's
   shade and MapDrawState's fill, folded into the picture as ((A - B) * C >> 7) + D a channel (`scene-slice2-results.md`):
   their pixels then differ from the GPU blend's (slice 1's) by a shade here and there. The owner accepts a picture that
   differs where it cannot be told apart; check that the changed pixels are nearer paraLLEl-GS, not further.
   Measured (`scene-slice2-after.md`): further, since the frame under the fills is a level off paraLLEl-GS already;
   so they are folded with the arithmetic of slice 2's blend (byte for byte), the GS's a developers' switch.

6. **The frame buffers an image each. Decided (2026-11-20).** Where both frame buffers are found as one target (from a
   New Game: the pond's 1920x1856), each frame's clear is a partial render area, not a fast clear (`scene-slice2-after.md`);
   each frame buffer an image of its own, as "3. The targets" has it, a read across the two answered by the store.
   Built and measured the same day (`scene-slice2-after.md`): no gain (the pond 13.14 ms a game frame split, 13.10
   not), so not kept; the partial clears cost nothing measurable here.

Also taken with them, as the design has them: the scene built in the chain's order on the renderer's thread; the
texture store as the resolver with a bindless table; DATE by the first-draw image (slice 1 checks its assumptions:
`GsDraw_SetScreenAlpha(0)` just before the shadows, a fallback by a copy where not); the alpha's reset folded into the
clear where nothing reads the alpha before it (slice 1's census checks, slice 2 folds); pictures between from the
scene; scheduling by predicted cost, the game never held; the six slices in their order.

## Risks and what is not known

- The census ran headless at 30 at seven scenes; the map change, the map viewers, the save screen and the Dark
  Succubus' copy were not counted (the inventory's captures of them were made with native draw off).
- D16's rounding and the depth clamp on Intel and NVIDIA (above).
- The skinned models' first passes: which are blended by their own alpha (they stay two draws), not counted yet.

## Running it again

```sh
M=~/Documents/darker-reality-measurements/2026-11-19-scene-design
python3 tools/scene_census.py --out $M/census --disc ISO                     # the census (headless)
KF4_PC_DEPTH=gsexact python3 tools/map_tour.py --out $M/tours/pc_gsexact --positions 3 --no-capture --renderer pc \
    --extra "--native-scale 1 --no-native-widescreen --native-draw on --frame-rate 30"
KF4_PC_DEPTH=gsexact python3 tools/motion_compare.py --out $M/motion/gsexact --profiles 1x --no-240 --keep \
    --pc-extra "--native-draw on --frame-rate 30"
python $M/bin/depth_cmp.py $M/tours                                           # the tour by depth mode, the split
python3 tools/world_plain.py --out $M/cost --variants gsexact,unorm,gpu --scales 2 --rounds 2   # a real window
```

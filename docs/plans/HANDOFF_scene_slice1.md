# Handoff: the scene renderer, slice 1 (game frames at 30)

2026-11-19. For the next agent, after `measure/depth-by-layers` (with the design) is merged or on top of it. Read
`CLAUDE.md`, then `renderer-scene-not-gs.md` (the direction) and **`scene-renderer-design.md`** (the design: what
this slice builds and why). Before building, ask the owner for the decisions the design lists ("The owner's
decisions"); slice 1 needs only the first (depth: `D16_UNORM`, recommended). If the owner chose otherwise, stop and
say what changes.

## The mission

`--renderer scene` beside `pc` and `gs` (developers only): game frames at 30, drawn from a scene, as close to
paraLLEl-GS as `KF4_PC_DEPTH=unorm` draws them, at the GPU depth's cost. No pictures between yet (at rates above 30 a
picture between shows the last picture again), no folding, no batching: those are slices 2-4.

1. **Where it lives**: `port/gs/scene/` (its own files, `scn_*`), opened through `port/platform/gs_api.h` and `gs.c`
   as `pcr_*` is, with the same calls (packets, vblanks, pictures, the presenter's callbacks and scanout ring, save
   states as today's: the store's VRAM image, captures). `main_host.c`'s `--renderer` takes `scene`; settings and the
   overlay never pick it.
2. **The intake**: today's `Intake` (`port/gs/pc/pcr.cpp`) is the model: the GIF front end (`pcr_gif.*`), the texture
   store (`pcr_textures.*`) and the native draws' fetch (`render.h`, `rd_fetch`, `pcr_native.cpp`'s way of writing a
   batch's A+D packet through the front end) reused as they are. Share the code, don't copy it: move what both need
   out of `port/gs/pc/` (into `port/gs/shared/` or `port/render/`) where it is only `pcr`'s by its folder.
3. **The scene** (`scene-renderer-design.md`, "The scene"): built in the chain's order, closed at the vblank, handed to
   the drawing side. Passes by target; native draws as draws of the arena's meshes with their records
   (`vu1_record.*`); materials resolved by the store then (a texture decoded once into an image kept by its key, the
   CLUT's colours in it; the material holds it); the GIF primitives as 2D items; clears as the pass's load ops; local
   copies and the frame's copy as ops.
4. **Drawing**: one draw a native batch for now (as today's canvas, without its interlock, planes or copies), the
   world's shaders from `pcr_nd000.vert`, `pcr_nd280.vert`, `pcr_nd700.vert`, `pcr_ndripple.vert` and `pcr_prim.frag`
   built without exact Z; 2D with `pcr_prim.vert` / `.frag`; `pcr_blend.h`'s blends. Targets as images of their own
   (RGBA8; the reflection's 16-bit target as RGBA8 with the 5:5:5:1 arithmetic in the shader, as the canvas does).
5. **Depth**: `D16_UNORM` for PSMZ16/PSMZ16S, D32F for the others; the depth Z / zmax; the viewport's depth range 0 to 1
   with the depth clamp (`pcr_common.h`'s `PCR_F_PIXEL_CLAMP` path is the arithmetic); invariant positions; the GS's
   test as the GPU's. Today's `KF4_PC_DEPTH=unorm` is the proof (`pcr_canvas.cpp`, `depth_format_for`).
6. **DATE** by the first-draw image (the design's "DATE"): the shadow draws into an R16_SFLOAT image with a MIN blend
   (their number in the frame), Z tested, nothing else written; then one pass darkening the written pixels by the first
   shadow's FIX and colour with the GS's arithmetic and writing alpha 0x80. Check at each frame that
   `GsDraw_SetScreenAlpha(0)` came just before the shadows (`Entity_DrawPoolModels`); where not, DATE by a copy of the
   shadows' rectangle. The HUD's DATE sprite: from a copy of its own rectangle.
7. **The screen passes** as today: `pass_draw.cpp` makes their GIF, drawn as 2D (the frame read by the brightness pass
   and the tint is a copy of the frame's image at that point, as the canvas makes it).
8. **The unidentified 3D**: 63-333 triangles a frame of VU1 program 0x000 still come as PATH1 with native draw on
   (`scene-renderer-design.md`, "What still comes as GIF"; at 099 about 27 VU1 runs a frame, TEX0 at pages 394, 468 ...).
   Find their call site (a GS capture with native draw on: `kf4-gsreplay --list` shows `prog 000`; the census's
   `p1 tri tex blend ztest`); draw them as 2D items with `pcr_prim`'s arithmetic for now, and say what they are.
9. **Replays**: `kf4-gsreplay --renderer scene` (captures already hold the native draws: `KF4GS_ND_*`).

## What it must show

- **Pictures**: `map_tour.py --renderer scene` at 1x (`--native-scale 1 --no-native-widescreen --native-draw on
  --frame-rate 30`, `--positions 3`, 760 views) against paraLLEl-GS's tour (in the measurements folder): no more than
  UNORM's 7,008 pixels over 24 levels (and never more than today's rule's 8,845), view by view no view newly over
  0.4%; `motion_compare.py` (its 13 cases at 1x, 30) no further from paraLLEl-GS than `unorm`'s motion run; every
  scenario of `layer_inventory.py` draws (the map change, the save screen, the map viewers, the menus, the title,
  under water, the Succubus' copy).
- **GPU time** (a real window, the owner's settings, the Vega 7 on mains and the balanced profile; `world_plain.py`
  taught the `scene` renderer, or `layer_inventory.py --steps live`): the world at 099, the pond and the palace at 2x
  within 10% of `unorm`'s 2.85 / 6.11 / 6.22 ms a game frame; DATE under 0.1 ms; no surface copies but the passes'.
- **The census** (`KF4_PC_FRAME_CENSUS`, `tools/scene_census.py`) taught the scene renderer or kept on `pc`: the alpha
  read between the frame's clear and the reset (none, the design assumes; slice 2 folds the reset on it).
- ctest; ASan + UBSan run of a tour's first maps; the Vulkan validation layer on a few scenes.

## Risks

- Targets aliasing in ways the census did not reach (the map change's copy into 0x2BC0 and its fade, the map viewers,
  the menus' grab and field buffers): the store answers which target wrote a page last; follow it, and list what
  needed it.
- The menus' 32-bit Z on D32F (the depth study's first form lost the world behind the menus at 16 bits).
- Sharing today's code without changing `pc`'s pictures: `pc`'s default path must stay byte for byte (the depth
  study's `bin/scenes.sh` pictures and a tour's first maps).

## How to run things here

As `HANDOFF_scene_renderer_design.md` says: build `cmake --build out/linux-clang`; the disc
`/home/voicedrew/Documents/King's Field - The Ancient City (USA).iso`; scripted runs `--native-draw on`; output under
`~/Documents/darker-reality-measurements/` (a dated folder, in its README), never `/tmp`; no building while a tour runs;
real windows for timing, said to the owner first. The measurements this design used are in
`~/Documents/darker-reality-measurements/2026-11-19-scene-design/` (`tours/` links the depth study's paraLLEl-GS, rule
and UNORM tours). Commit on a branch; ask the owner before pushing.

# Handoff: the scene renderer, slice 3 (batching), in five parts

2026-11-20, after slice 2 and what followed it (`scene-slice2-results.md`, `scene-slice2-after.md`, on `scene/slice2`
at `d45c9b4a`). Read `CLAUDE.md`, then `renderer-scene-not-gs.md` (the direction), `scene-renderer-design.md` ("5.
Draws", "10. The slices", the owner's decision 2) and `scene-slice2-after.md` ("Slice 3's start").

Slice 3 is too much for one session: new shaders and plumbing, a fused shader the owner must look at, and the whole
slice measured. It is cut into five parts. Each part is a session or so, ends with the build working, the pictures
checked and a commit, and writes its numbers into `scene-slice3-results.md` (made by the first part, one section a
part) so the next session starts from the document, not from memory.

## The slice (from the design)

Bindless textures; runs of one pipeline as indirect draws; program 0x280's two passes as one draw where exact. Passes
when: pictures byte for byte slice 2's where claimed exact; the fused passes' pixel changes counted and shown to the
owner; 100-250 calls a frame; the world at 099 under 2 ms, the pond and the palace's 0x280 under 1 ms (2x, the Vega 7,
a real window with the owner's settings). Risks: the fused passes' overlap; drivers' indirect draw speed on integrated
GPUs.

## The parts

| part | what | depends on | done when |
|---|---|---|---|
| 3a | records and the bindless table, still a call a draw; the counts; the baseline | slice 2 | byte for byte slice 2's; GPU time no worse |
| 3b | runs as indirect draws (all but 0x280's alternation) | 3a | byte for byte; calls counted; 0x000 at 099 under 2 ms |
| 3c | the level off paraLLEl-GS at 1x (a study) | nothing | found, or what is ruled out written down |
| 3d | 0x280's two passes as one draw, behind a switch | 3b; 3c before it is judged | the changes counted and pictured for the owner; 0x280 under 1 ms |
| 3e | the slice judged whole; the owner's decision on 3d | 3a-3d | the results document complete; slice 4's start written |

3c touches no renderer code and can be done at any point before 3d is judged (first, if a session is short of a
whole part).

### 3a. Records and the bindless table

The plumbing alone, so that a fault here is not mixed up with batching's.

- **The baseline first**: `world_plain.py --variants scene` at 2x (099, the pond, the palace, two rounds), once with
  `KF4_GPU_DETAIL` as slice 2 measured it and once with it off (a run's GPU time a game frame only): the detail's
  timestamps wait for the GPU at each change of shader state, and 0x280 changes state with every pass, so slice 3's
  gains must be read with it off too (`scene-slice2-after.md`, "Slice 3's start").
- **The counts**, in `[stats] scene renderer`: calls a frame, runs of one pipeline (textures bindless), of them 0x280's,
  and what ends a run (a material change, a pass, a draw that must keep its own descriptors, the cut triangles of
  0x000 that come as GIF: `scene-renderer-design.md`, "What still comes as GIF"). Check them against the design's
  census (`5. Draws`: 207 / 1,020 / 1,523 runs at 099 / the pond / the palace).
- **Records**: each draw's fields (today's push constants and the material's constants) in a records buffer the scene
  holds, read by `gl_DrawID` as the PC renderer's `PCR_RECORDS` shaders read them (`pcr_common.h`; `pcr_canvas.cpp`'s
  `Runs` is the model). Shared shaders, not copies.
- **The bindless table**: each texture the store resolves for the frame put in a table the scene holds (the Drawer's
  `table_pool_` is a one-entry dummy today), its index in the draw's record; the CLUTs in one buffer (`clut_at`).
  Draws that read the frame or a copy (`u_surface`, `u_dest`: the HUD's DATE sprite, the shader's blends, the picture's
  layers) keep their own set 1 and end a run.
- Still a call a draw: nothing is batched yet.
- **Passes**: `scene_scenarios.py --reference-exe` against slice 2's end (`d45c9b4a`): the 50 scenarios byte for byte
  (but "start spin", which differs between two runs of itself); the validation layer with synchronization validation
  on start, the pond, the palace, fx prev and the map change; ctest; GPU time within the baseline's spread (what
  sampling through the table costs, measured).

### 3b. Runs as indirect draws

- Consecutive draws of one pipeline and dynamic state as one `vkCmdDrawIndirect` (a command each, the record the
  first instance) for 0x000 (the map, objects, items, and the shadows' first-draw and darken passes), 0x700, the ripple,
  and the GIF's 2D by state; 0x280 too, though its two passes alternating leave it little to gain until 3d.
- The PC renderer has the indirect runs and the merged runs (one `vkCmdDraw` of several batches' triangles): if
  indirect draws are slow on the Vega 7 (the design's risk), measure the merged form against them and keep the faster.
- **Passes**: byte for byte as 3a; calls a frame counted against the design's 20-70 for 0x000; 0x000 at 099 under 2 ms
  at 2x (2.6-3.0 in slice 2, with the detail on). If the cut triangles of 0x000 (GIF, about 18 places a frame at 099)
  split runs enough to matter, say how many; moving them into the native draw is a later step, not this part's.

### 3c. The level off paraLLEl-GS at 1x

At 1x the scene renderer is about a level off paraLLEl-GS under every picture: the palace -0.98 a channel with 16% of
channels exact, 099's world about +0.5 (`scene-slice2-after.md`, "What is left"; `scen_gs2/` in
`~/Documents/darker-reality-measurements/2026-11-20-scene-alpha-fills/`). It is not counted by the tours (pixels over
24), but 3d's changes are of the same size, so judging 3d against paraLLEl-GS needs it found or at least bounded.

- Where it comes from: the texture's filtering, the shading's interpolation and rounding (Gouraud, fog, the
  colour's scale), the brightness's (y * 136) >> 7, the final picture. One frame's pixels taken apart by stage, the
  scene renderer's against paraLLEl-GS's, and the PC renderer's beside them: if it has the same level, the cause is in
  the code the two share.
- A session at most. If not found, write down what is ruled out, and judge 3d against slice 2 byte by byte and against
  paraLLEl-GS with the level noted.
- A fix that moves pictures is a change the owner judges ("Indistinguishable picture changes are OK": nearer
  paraLLEl-GS, never further); behind a developers' switch until then.

### 3d. 0x280's two passes as one draw

- Where a 0x280 batch's first pass is unblended or its blend leaves the source alone (every sphere batch counted at the
  pond and the palace: colour scale.w 0, bias.w 128, the first pass's alpha 0x80), one draw: the first pass's colour
  made in the shader and the sphere pass's (Cd * As2 >> 7) + Cs2 over it with the GS's integer arithmetic, written
  once, no blend. Where it does not apply, two draws as now. Count both.
- Behind a switch (`KF4_SCN_FUSE_280`), **off by default** until the owner has looked (decision 2).
- **Measured**: the batches it applies to; the pixels it changes against the two passes drawn apart (the 50 scenarios
  at 2x, the tour and `motion_compare.py` at 1x), against paraLLEl-GS too, with 3c's level noted; pictures of the
  changed pixels (each case's picture, the difference marked) for the owner; then the runs and calls a frame (the design's
  100-250) and 0x280 at the pond and the palace under 1 ms.

### 3e. The slice judged whole

- The slice's criteria, each with its numbers, in `scene-slice3-results.md`: the tour at 1x against paraLLEl-GS (no
  further than slice 2's), `motion_compare.py`, the 50 scenarios, ASan and UBSan on a tour's first maps, the
  validation layer, `world_plain.py` at 2x in a real window with the detail off and on, the Vega 7 and the 9070 XT
  (the owner's).
- The owner's decision on 3d recorded in `scene-renderer-design.md`'s decisions (kept on by default, or not), and the
  switch's default set by it.
- Indirect draws' speed on an integrated GPU other than the Vega 7, if one is to hand; else listed as not checked.
- What slice 4 (pictures between) starts from.

## How to run things here

As slice 1's handoff says: build `cmake --build out/linux-clang`; the disc
`/home/voicedrew/Documents/King's Field - The Ancient City (USA).iso`; scripted runs `--native-draw on`; output under
`~/Documents/darker-reality-measurements/` (a dated folder a part, `2026-MM-DD-scene-slice3-<part>`, in its README),
never `/tmp`; no building while a tour runs; real windows for timing, said to the owner first. Work on `scene/slice3`,
branched from `scene/slice2`'s end; the reference build for byte compares is slice 2's end (`d45c9b4a`), built once
into its own folder. Commit at the end of each part; ask the owner before pushing.

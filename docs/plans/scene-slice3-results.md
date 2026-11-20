# The scene renderer, slice 3: results

Slice 3 of `scene-renderer-design.md` ("The slices"): batching, cut into five parts (`HANDOFF_scene_slice3.md`), on
`scene/slice3` from slice 2's end (`d45c9b4a`). A section a part, written as each is done. The measurements are in
`~/Documents/darker-reality-measurements/2026-11-20-scene-slice3-<part>/` (their README entries).

## 3a. Records and the bindless table

2026-11-20 (`2026-11-20-scene-slice3-3a/`). The plumbing alone, still a call a draw's pass.

### What it does

- **Records.** Each scene recorded gets a records buffer (`scn_draw.cpp`'s `prepare`): a record a draw's pass, the
  fields slice 2 pushed as push constants (`pcr::PrimPush`), in the draws' order. A draw pushes only its record's place
  and is drawn by the PC renderer's shaders built again with `PCR_RECORDS` and `PCR_DRAWS_APART` (`scn_shaders.h`'s
  `scn_pcr_*_rec`, and the scene's two DATE shaders so: shared, not copied): each record a draw of its own, no merged
  runs, so both stages load the record's fields at the push constants' place (+ `gl_DrawID` in the vertex stage), the
  same across the draw. A shadow's first triangle number is its record's own `record` field. The PC renderer's shaders
  are unchanged (`pcr_shaders.h` byte for byte as before).
- **The bindless table.** Every texture the store decoded or sampled for the scene's draws (`u_texels`, `u_sampled`)
  goes in one table a scene (set 2, `u_table`; the Drawer's one-entry dummy before), its place and its sampler's (the
  scene's sixteen, set 0's `u_samplers`) in the draw's record; a sampled texture's size in its record's `src_offset`
  (`pcr_prim.h`'s `sampled_size`). The CLUTs are one buffer (`u_walk_cluts`, the record's `clut_at`).
- **Draws that keep their own set 1** and push constants, and so end a run: those reading a target or its copy
  (`u_surface`: the scrolling textures, the reflection, the sky; `u_dest`: the HUD's DATE sprite, the shader's
  blends), the picture's layers (`scn_picture_layer.frag`), and a material whose sampler finds no place among sixteen
  (none met).
- **The counts.** `[stats] scene renderer calls`: calls (a draw's pass each), of them from the records; runs of calls
  one indirect draw could draw (consecutive in a pass, the same program, depth, blend, write mask, blend constants,
  viewport, scissor and native buffers: textures bindless), of them 0x280's; what ended each run (its pass, a change
  of state, its own descriptors, a clear, the GIF's 3D: `Draw::gif_3d`, GIF triangles Z tested, which are program
  0x000's triangles cut where they cross the GS's range); the runs the GIF's 3D cut in two (the same state either side).
  `KF4_SCN_RUNS_LOG=FILE` writes them a scene recorded (`runs_at.py` in the measurements' `bin/` takes the medians at a
  place).
- Developers' switch: `KF4_SCN_RECORDS=0`, each draw with its own push constants and set 1 as slice 2
  (`world_plain.py`'s `scene_norecords`).

### The counts at the three places

Headless at 2x with native widescreen, the scenarios' last 100-500 vblanks before their pictures (`runs/`), medians a
game frame (the frame's scenes; the pictures' are a few calls more):

| | 099 (start) | the pond | the palace |
|---|---|---|---|
| calls (a draw's pass each) | 361 | 1,564 | 2,029 |
| of them from the records | 357 | 1,562 | 2,028 |
| runs of one pipeline, textures bindless | 243 | 1,053 | 1,652 |
| of them 0x280's | 139 | 885 | 1,437 |
| the design's census: runs (of them 0x280's) | 207 (139) | 1,020 (927) | 1,523 (1,437) |
| a run ended by: a change of state | 221 | 1,037 | 1,640 |
| ... its pass | 10 | 8 | 9 |
| ... its own descriptors | 3 | 2 | 1 |
| ... the GIF's 3D | 9 | 6 | 2 |
| runs the GIF's 3D cut in two | 3 | 1 | 1 |
| textures in the table | 38 | 47 | 48 |

- **0x280's runs are the census's** (139, 1,437; the pond's 885 against 927: another moment of the walk). The other
  runs are 30-130 more than the census's, which counted the GS's state (`pcr_frame_census.cpp`'s `pipeline_of`) where
  these count Vulkan's: an alpha-tested material's two passes are two calls with two write masks, and viewport,
  scissor and the native programs' buffers are state here.
- **What ends the runs is state**, nearly all of it 0x280's passes alternating (3d's). Own descriptors end 1-3 a frame,
  passes 8-10. The GIF's 3D ends 2-9 and cuts 1-3 runs in two: not enough to matter for 3b.
- No CLUT reaches the records in play: the indexed textures are sampled (`colour_texels`), their colours in the
  table's image. With the GS's reads (`KF4_PC_SAMPLER=0`) they do, 20-306 a scene.

### Passes

| criterion | measured | passed |
|---|---|---|
| Byte for byte slice 2's: the 50 scenarios at 2x (`scene_scenarios.py --reference-exe` slice 2's end, `scen_records/`) | 49 of 50; "start spin" differs (1.38 million pixels), as it does between two runs of slice 2 (its mouse spin) | yes |
| ... with the GS's texture reads (`KF4_PC_SAMPLER=0`, both builds): the raw texels from the table and the CLUTs from their buffer, which no scenario reaches otherwise (every indexed texture is sampled) | title, start, the pond, the palace, the menus, fx prev: 6 of 6 byte for byte, 20-306 CLUTs a scene (`scen_nosampler/`) | yes |
| The validation layer with synchronization validation: start, the pond, the palace, fx prev, the map change, fx fill, pickup (`validation/`) | no error or warning; every picture byte for byte slice 2's | yes |
| ctest | 37 of 37 | yes |
| GPU time within the baseline's spread (real window, 2x, Vega 7, two rounds; below) | 099 and the pond within it (+0.04 / -0.01 and +0.01 / -0.04 ms a game frame, the detail on / off); the palace 0.12 ms over (+0.12 / +0.14 over four rounds, 0.1 of it 0x280's), outside its 0.01-0.08 | not at the palace, by 0.12 ms |

### The baseline and the GPU time

`world_plain.py --scales 2 --rounds 2` in a real window with the owner's settings (Vega 7, 30), ms a game frame,
medians (`cost/`; the palace again, two rounds more, `cost_palace/`). `scene_ref` is slice 2's end, `scene` this build,
`scene_norecords` this build with `KF4_SCN_RECORDS=0`; `_nodetail` the same with `KF4_GPU_DETAIL=0` (the GPU's time by
kind alone: the detail's timestamps wait for the GPU at each change of shader state; its "world" is then the native
kinds whole, DATE and the sky in it).

| scene | slice 2 | 3a | 3a, records off | slice 2, detail off | 3a, detail off |
|---|---|---|---|---|---|
| 099 | 10.41 | 10.45 | 10.41 | 10.43 | 10.42 |
| the pond | 13.12 | 13.13 | 13.15 | 13.14 | 13.10 |
| the palace | 12.81 | 12.90 | 12.83 | 12.69 | 12.91 |
| the palace, two rounds more | 12.82 | 12.94 | | 12.79 | 12.93 |
| the world at the palace (0x000; 0x280 and 0x700) | 3.70; 2.64 | 3.69; 2.75 | 3.68; 2.64 | | |

The spread between rounds: 0.00-0.08. **The baseline** (slice 2 with the detail off) is the same as with it on to
0.12 ms: at 30 at these places the detail's waits cost little in a game frame's whole, though they move time between
the kinds (099's 0x000 3.54 with it off against 2.61 on: off, the 2D's and the sky's work before the next timestamp is
counted in it).

- **What the records and the table cost**: nothing measurable at 099 and the pond; 0.1 ms at the palace, in 0x280:
  its skinned models' first pass (sampled, modulated, fogged, blended by As; 2.09 ms a game frame) takes 0.08 more,
  4%: a draw's cost, on about 1,400 small calls a frame, of reading its record and its texture through the table where
  slice 2 had push constants and a set of its own. 3b's indirect draws and 3d's fused passes both take most of those
  calls away; it is measured again there.
- **On the way: the PC renderer's records cost the scene 0.5-1.0 ms a game frame.** The first build drew with the PC
  renderer's `PCR_RECORDS` shaders as they are (`first_build/cost/`): 099 +0.64, the pond +1.05, the palace +1.00, the
  detail on or off, all of it in the native draws. Two causes, each measured in a window: (1) the merged runs' record
  (`pcr_merged_record`, a value of each vertex) made the record's place differ by vertex, so every field was loaded a
  vertex, and the table's index nonuniform: built without them (`PCR_DRAWS_APART`), 0.1-0.2 back; (2) the fields the
  fragment stage reads, handed down from the vertex stage as 25 flat values a vertex (`v_record`): the rest. The
  scene's fragment stage now reads its record at the push constants' place itself, loaded once a draw.

### What 3b starts from

- **The fragment stage's record in an indirect draw.** It reads its record at the push constants' place, which is
  right while each record is a call of its own. An indirect draw of several records has no `gl_DrawID` in the fragment
  stage, and handing the fields down as the PC renderer does is the 0.5-0.8 ms above. 3b must find a way that costs
  about what 3a does: one flat value a vertex (the record's place) and the loads by it, scalarised where the place is
  the same across a wave (it is, within a draw), measured against 3a; or the fields the fragment stage needs most kept
  few. The vertex stage is fine (`gl_DrawID`).
- The runs (above): 243 / 1,053 / 1,652 a frame at 099 / the pond / the palace, 139 / 885 / 1,437 of them 0x280's
  alternation; outside 0x280, 104 / 168 / 215, against the design's 20-70 for 0x000 (these count the alpha test's two
  passes, viewport, scissor and buffers as state; 3b sees which of them it can draw as one).
- The GIF's 3D ends 2-9 runs a frame and cuts 1-3 in two: not worth taking into the native draw for 3b.

## 3b. Runs as indirect draws

2026-11-20 (`2026-11-20-scene-slice3-3b/`).

### What it does

- **The fragment stage's record.** The vertex stage hands down the record's place alone, one flat value
  (`pcr_common.h`'s `v_record_at`: the push constants' record + `gl_DrawID`). The fragment stage runs its shader for
  one record at a time (`PCR_EACH_RECORD`): the subgroup's first pixel's record (`subgroupBroadcastFirst`), by the
  pixels that have it, then the next, until none is left. A draw's pixels and their quads share a record, and a
  subgroup is nearly always one draw's, so it is once round: the record's place the same across the subgroup, its
  fields loaded once and its tests the subgroup's, as with 3a's push constants. It costs what 3a did (below), not the
  0.5-0.8 ms of the 25 flat values. The PC renderer's shaders unchanged (`pcr_shaders.h` byte for byte). Needs draw
  parameters and the subgroup's ballot in the fragment stage, else no records (a draw's own push constants, slice 2's).
- **The runs.** Consecutive calls from the records of one pipeline and dynamic state, their records consecutive, are
  one `vkCmdDrawIndirect` (`scn_draw.cpp`'s `Pending`): a command a call in a commands buffer the scene holds, written
  as the calls are recorded, the push constants' record the run's first. A run is drawn when a call cannot join it
  (another state, a clear, a draw of its own, the pass's end); a run of one call is a `vkCmdDraw`. The GIF's primitives
  drawn from the records read the scene's corners from one vertex buffer (each draw's first vertex its command's), no
  longer copied a draw. Needs multi-draw indirect and the first instance in indirect draws (the native draws'
  instance), else a call a `vkCmdDraw`.
- **State in one form** (`canonical`): a key's alpha blend where the alpha is not written, and the blend constants where
  no factor reads them, do not end a run (the pixels the same either way).
- **Unblended calls in runs blended by the source's alpha** (`merge`). What ended most of 0x000's runs was its models'
  opaque batches (no blend, RGB written) alternating with their batches blended by As (`SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA`)
  on the same program and buffers. An unblended pass that writes no alpha has its alpha out 1.0 from its record
  (`SCN_ALPHA_ONE`, `pc.factors`, unused by the scene's records before), so it is drawn the same under that blend
  (Cs 1 + Cd 0 is Cs) and joins such a run; a run of them meeting a blended call is drawn blended. Byte for byte below.
- Developers' switches: `KF4_SCN_INDIRECT=0` (every call its own `vkCmdDraw`, the same shaders), `KF4_SCN_BLEND_RUNS=0`
  (no unblended call in a blended run); `world_plain.py`'s `scene_noindirect`, `scene_noblendruns` and their
  `_nodetail`. `[stats] scene renderer draw calls` and the runs log's new columns (`draw_calls indirect_calls
  indirect_draws calls_gif calls_000 calls_280 calls_700 calls_ripple`) count the calls made.

### The calls at the three places

As 3a's counts (headless at 2x with native widescreen, the scenarios' last 100 vblanks before their pictures, medians a
game frame; `runs_merge/`, and `runs/` for the build without `canonical` and `merge`):

| | 099 (start) | the pond | the palace |
|---|---|---|---|
| a draw's pass each (3a's calls) | 361 | 1,562 | 2,029 |
| draw calls, 3a | 361 | 1,562 | 2,029 |
| draw calls, runs of the same state | 243 | 1,055 | 1,652 |
| draw calls, 3b (with `canonical` and `merge`) | 171 | 971 | 1,478 |
| of them indirect (their commands) | 13 (203) | 82 (676) | 14 (565) |
| ... program 0x000's | 13 | 55 | 25 |
| ... 0x280's | 139 | 885 | 1,437 |
| ... the GIF's | 18 | 9 | 15 |
| ... 0x700's and the ripple's | 1 | 22 | 1 |
| 0x000's, runs of the same state alone | 84 | 139 | 199 |

- **0x000's calls are the design's 20-70** (13 / 55 / 25), from 84 / 139 / 199 with the state compared as it is: the
  blend's alternation was most of them.
- **What is left is 0x280's** passes alternating, 81-97% of the calls: 3d's.

### Passes

| criterion | measured | passed |
|---|---|---|
| Byte for byte 3a's: the 50 scenarios at 2x (`scene_scenarios.py --reference-exe bin/kf4_3a`) | first build (`scen_3b/`): 49 of 50, "start spin" as ever; the build committed (`scen_final/`): 49 of 50, "start spin" again | yes |
| The validation layer with synchronization validation: start, the pond, the palace, fx prev, the map change, fx fill, pickup (`validation/`) | no error or warning; every picture byte for byte 3a's, 6,800-20,500 indirect draws a run | yes |
| ctest | 37 of 37 | yes |
| Calls a frame counted, 0x000's against the design's 20-70 | 13 / 55 / 25 (above) | yes |
| 0x000 at 099 under 2 ms at 2x (real window, Vega 7) | 2.60 with the detail on (3a 2.61; slice 2 2.61): the calls were not its cost (below) | **no** |
| GPU time no worse than 3a | the same at 099 and the pond (within the rounds' spread), 0.13-0.15 ms less at the palace | yes |

### The GPU time

`world_plain.py --scales 2 --rounds 2` in a real window with the owner's settings (Vega 7, 30), ms a game frame,
medians (`cost/`). `3a` is `bin/kf4_3a`; the detail off is `KF4_GPU_DETAIL=0` (its 0x000 then has DATE and the sky in
it).

| | 3a | 3b | 3b, a call a `vkCmdDraw` | 3b, no blended runs |
|---|---|---|---|---|
| 099: 0x000, detail on / off | 2.61 / 3.53 | 2.60 / 3.54 | 2.63 / 3.56 | 2.61 / 3.53 |
| 099: all, on / off | 10.43 / 10.41 | 10.42 / 10.43 | 10.47 / 10.48 | 10.47 / 10.46 |
| the pond: 0x000; 0x280 and 0x700 (on) | 2.84; 3.94 | 2.85; 3.96 | 2.88; 3.98 | 2.89; 3.98 |
| the pond: all, on / off | 13.14 / 13.07 | 13.18 / 13.12 | 13.19 / 13.20 | 13.20 / 13.15 |
| the palace: 0x000; 0x280 and 0x700 (on) | 3.68; 2.74 | 3.58; 2.68 | 3.71; 2.75 | 3.71; 2.72 |
| the palace: all, on / off | 12.89 / 12.86 | 12.74 / 12.73 | 12.94 / 12.97 | 12.93 / 12.87 |

The spread between rounds 0.00-0.09 (the palace without blended runs 0.15).

- **The fragment stage's record a record at a time costs up to about 0.1 ms** against 3a's push constants' place: 3b
  with a call a `vkCmdDraw` (3a's calls, 3b's shaders and the GIF's corners from one buffer) is 0.04-0.13 over 3a, a
  little past the rounds' spread with the detail off (+0.07 / +0.13 / +0.11 at 099 / the pond / the palace), not with
  it on (+0.04 / +0.05 / +0.05). The runs win it back: 3b as a whole is 3a's or better everywhere. Not the 0.5-1.0 of
  the 25 flat values.
- **The indirect draws gain 0.01-0.24 ms a game frame, the blended runs up to 0.19**, both most at the palace (3b 0.20 /
  0.24 under a call each, 0.15 / 0.13 under 3a): 3a's 0.12 there is won back, and the palace is slice 2's again
  (12.74 / 12.73 against 12.81 / 12.69). The blended runs put opaque batches under a blend (a destination read) and
  cost nothing for it: within the spread at 099 and the pond, 0.19 / 0.14 less at the palace.
- **Indirect draws are not slow on the Vega 7** (the design's risk): the merged form was not needed.
- **0x000 at 099 is not under 2 ms, and fewer calls will not make it so.** Its calls went from 84 to 13 a frame and
  its time from 2.61 to 2.60 ms; a call each against the runs is 0.03 ms. The time is the GPU drawing 0x000's pixels at
  2x, not the calls. What in them (the fragment shader's length, the texture's reads, overdraw, the depth's
  interpolation) is the next question, not batching's: until it is answered, the criterion stands failed.

### Risks and what 3c-3e start from

- **RADV and indirect draws.** The PC renderer turned its indirect runs off on RADV (`pcr_canvas.cpp`'s
  `runs_hang_here`): a ring timeout and a reset of the whole card on the RX 9070 XT at 240 pictures a second, in 4 of
  6 runs. The scene renderer draws at the game's rate here (no pictures between yet), and no hang was met on the Vega 7
  in this part's runs, but the 9070 XT was not tried: 3e must, with the owner's settings, before the runs are kept on
  there. `KF4_SCN_INDIRECT=0` is the way back.
- 3d: 0x280's alternation is 81-97% of the calls left (139 / 885 / 1,437 a frame).
- 0x000's time at 099 (2.6 ms with the detail on) is not in its calls: a study of where it goes (as 3c's of the level)
  is needed before the slice's "under 2 ms" can be judged.

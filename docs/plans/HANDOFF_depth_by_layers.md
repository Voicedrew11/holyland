# Handoff: depth by layers, a proof of concept

2026-11-19. For the next agent, on branch `measure/world-plain` (or after it is merged). Read `CLAUDE.md` first, then
`renderer-scene-not-gs.md` (the plan, revised today) and `world-plain.md` (the measurements this builds on).

## The mission

Prove or disprove the idea the scene renderer's depth rests on, **before** the scene renderer is built: that the
GS-exact depth (half the world's GPU time) can go, because the draws that can tie are known from the game's data and
everything else can use the GPU's own depth test, with pictures the owner accepts.

The owner wants the port **extremely fast on any GPU** (1080p on an integrated one), by general fixes, never per-GPU
tuning or "lower your settings" (memory: `perf-general-not-hardware`). Pictures stay exact. Exactness for depth is the
open question this step answers with numbers: the owner decides from them.

## Why this step

Measured on the owner's Vega 7 at 2x (`world-plain.md`), GPU ms a game frame:

| | 099 | Pond | Palace |
|---|---|---|---|
| The world today (the depth rule, exact Z's planes) | 5.55 | 10.22 | 9.18 |
| The world with the GPU's depth test (`KF4_PC_DEPTH=gpu`) | 2.83 | 5.87 | 6.07 |
| Exact Z alone, no rule (`KF4_PC_DEPTH=exact KF4_PC_DECAL_SETTLE=0`) | 4.33 | 9.91 | 8.53 |

Plus 1.4 ms outside the world (the depth copy's clears, DATE's marking), and all of it again in every picture between.
Nothing else GS-shaped costs anything in the world (targets, layouts, compression, blends).

The GS's Z is a whole number from each triangle's plane, and a later draw wins a tie (GEQUAL). The game relies on ties:
decals and layers drawn over their surface as meshes of their own (the moss on 063's low wall, 028's puddle, 099's
rock shading, 024's lava edges, 009's fire, 280's decals). A replay of the GS's stream cannot tell which triangles can
tie, so the depth rule tests every pixel of every draw GS-exactly, in draw order (interlock, a copy of the depth).
Every cheaper way tried failed because it guessed ties from an anonymous stream (`PC_RENDERER.md`: the decal nudge,
the draw-number tie, the settled decals; "Exact Z" and "The depth rule" there).

The claim to test: **the ties are knowable** (from the map's records and the models, before drawing), and with them
handled, the GPU's depth gives pictures that differ from paraLLEl-GS only where two different surfaces cross (the GS
rounds Z to whole numbers, the GPU keeps a fraction), in few enough pixels to accept.

## The questions, in order

1. **Which draws actually tie, in play, at every map?** Ground truth first, from the renderer that already knows: the
   depth rule decides a tie in its shader (`pcr_prim.h`'s `pcr_depth_rule`: within twice the tolerance, the GS's test
   decides). Count, per draw (batch, its program, its record or model, `is_decal`, its blend), the fragments it won
   or lost by a tie, at a map tour's views (`map_tour.py`, every map) and the motion cases (`motion_compare.py`'s, for
   the creatures and effects). A developers' counter in the rule's shader (an atomic per batch, read back) or a
   variant of `--decal-losses` (`kf4-gsreplay`, which already draws where decals lost a near tie) would do.
2. **Are those draws identifiable before drawing?** Against the data: is every tying draw `is_decal` (blended, GEQUAL,
   not covering: `pcr_canvas.cpp`), a layered map record, a known model part (0x280's decals: about 1,900 at 009's
   fire)? Which ties are not (an opaque record coplanar with another, a model on the floor)? The native-GS era's PC
   world (P2, P7 in `PC_RENDERER.md`) found the map's blended layers were 58% of its batches at 009 and drew "coplanar
   layers by order and a depth bias" with `invariant gl_Position`: read what it found before building.
3. **What does the GPU's depth with the ties handled cost and change?** A developers' depth mode on today's renderer
   (`KF4_PC_DEPTH=layers`, beside `rule|exact|gpu` in `pcr_canvas.cpp`'s `depth_asked`): the GPU's depth for every
   draw, the tying draws found in 2 handled the PC way. The stream's order already draws a layer after its surface, so
   the simplest form is a depth bias on those draws (a layer wins against its own surface, loses to anything really
   nearer); the stricter form gives a layer its surface's depth exactly (the same plane, or EQUAL), which needs to know
   which surface: try the bias first and say what it gets wrong. Check that the native vertex shaders give the same
   depth for the same corner in every draw (no current PC shader declares `invariant gl_Position`).
   - **Cost**: `world_plain.py` with a `layers` variant at 099, the pond and the palace, 1x/2x/4x, against `exact` and
     `gpu` (2 rounds).
   - **Pictures**: a map tour with `--renderer pc` in the new mode against one with `--renderer gs` (paraLLEl-GS), by
     `tour_compare.py`, beside today's (`rule`) against the same `gs` tour, and the motion cases by
     `motion_compare.py`. What matters is what the mode adds to today's differences: how many pixels, where (crossing
     surfaces? a layer lost? a layer through something?), each kind explained with a picture.
4. **The world's overdraw** (`world-plain.md`'s open question 1): how many fragments the world shades a game frame
   against its pixels, and whether the game draws back to front: a pipeline statistics query
   (`VK_QUERY_TYPE_PIPELINE_STATISTICS`, fragment shader invocations; Granite has none, so through the device table
   as `pcr_canvas.cpp`'s barriers are, with `pipelineStatisticsQuery` enabled) around the native batches, and the
   batches' Z ranges in order. It says how much drawing front to back would save (the floor, 1-1.5 ms at 2x).

## The deliverable

`docs/plans/depth-by-layers.md`: which draws tie (per map and per kind, how many fragments), whether the data names
them ahead, the layers mode's GPU ms against `exact` and `gpu`, its pictures against paraLLEl-GS beside today's (with
examples of every kind of difference), the overdraw, and a recommendation the owner can decide on: what "exact" means
for the scene renderer's depth. The mode and the counters stay developers' switches; nothing a player gets.

## Where to start

- The depth paths: `pcr_canvas.cpp` (`depth_asked`, `exact_z_`, `rule_on_`, `settle_on_`, `is_decal`,
  `blend_covers`, `draw_batch`'s `rule` and `flags`), `pcr_prim.h` (`pcr_depth_rule`, `pcr_exact_depth`,
  `pcr_full_depth`), `pcr_plane_pass.*` and `shaders/pcr_planes_*.comp`, `shaders/plane_z.h`, `pcr_common.h` (why
  exact Z).
- The native draws: `pcr_native.cpp` (programs 0x000, 0x280, 0x700, the ripple), `game/native_draw/` (what each draw
  is: the map's records, objects, entities, the arm, the sky, the water, the effects), `nd_skinned.h`.
- The history, so as not to repeat it: `PC_RENDERER.md`, "Exact Z (2026-11-13)", "The fine part and decals", "The
  depth rule (2026-11-15)", "Layer Z", and the native GS's PC world (P2, P3, P7: `pc_world_flush`'s "coplanar layers by
  order and a depth bias").
- Tools: `tools/world_plain.py` (costs; add a variant), `map_tour.py`, `tour_compare.py`, `motion_compare.py`,
  `replay_compare.py --decal-losses`, `kf4-gsreplay --pixel X,Y` (the pixel trace: every draw's fragments at one pixel, each sample's Z against the Z there) (tools/README.md for each).
- The measurements so far: `world-plain.md`, `world-plain-results.md`; raw runs and the inventory's GS captures in the
  owner's `~/Documents/darker-reality-measurements/` (its README; the captures replay with `kf4-gsreplay` and give
  many scenes without playing).

## How to run things here

- Build: `cmake --build out/linux-clang`. Disc: `/home/voicedrew/Documents/King's Field - The Ancient City (USA).iso`
  (pass `--disc`). Tests: `ctest --test-dir out/linux-clang`.
- Timing in a **real window** with the owner's settings (`CLAUDE.md`, "Testing graphics"; `world_plain.py` does it),
  on mains power, the balanced profile as before, nothing else on the GPU, and no building while it runs (the APU's
  CPU and GPU share their power). A run is about 14 s; say when windows will open.
- Pictures headless and byte for byte (`--dump-frames`, the tours).
- The GPU-time sums overcount where submissions overlap (4x, 60): compare like with like (`world-plain.md`, "What
  the sums mean").
- `decomp/` is read-only. Commit on a branch; ask the owner before pushing.

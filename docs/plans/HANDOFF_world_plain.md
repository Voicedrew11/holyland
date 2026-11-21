# Handoff: measure the world drawn plainly

**Done 2026-11-19: `world-plain.md`.**

2026-11-19. For the next agent, on branch `inventory/fullscreen-layers` (or after it is merged). Read `CLAUDE.md` first.

## The mission

Make the PC renderer work like a PC game's, not an emulation of the PS2's Graphics Synthesizer. King's Field IV runs
natively and draws a few thousand triangles a frame: it should be **extremely fast on any GPU**, 1080p on an
integrated GPU included. The owner wants general fixes to how the port draws, never per-GPU tuning or "lower your
settings" (memory: `perf-general-not-hardware`). Pictures stay exact: every step is checked byte for byte against
`main` (`--dump-frames`) before it ships. Done right, the owner's laptop (Ryzen 5 5625U, Vega 7) should do several
hundred pictures a second at 2x; today it struggles to hold 60.

The direction and its steps: `docs/plans/renderer-scene-not-gs.md`. Why the game chokes: `camera-whip-slowdown.md`.
What the GS emulation costs, measured: `fullscreen-layers-inventory.md` (step 1, done).

## Why this step

The inventory accounted for the full-screen layers (a quarter of a game frame) and the costs around them (pictures
between redrawing everything, the pump's surface copies, DATE's set-up, the final step's passes). What it did not
measure is **the world itself**: about 6.5 ms a game frame at the owner's settings in the real window (`start` in the
inventory's "The real window" table; the `native 000 ...` rows of `KF4_GPU_DETAIL=1`). It is drawn the GS's way:

- **exact GS depth:** the depth rule (`pcr_canvas.cpp`, `rule_on_`, "the shader tests the depth: fragment shader
  interlock" at start-up): depth tested in the shader against a copy kept in a storage image, cleared with every clear
  (`clear_depth_copy`); or exact Z with decals settled (`settle_decal_depth`);
- **blends the GPU cannot do** done in the shader from a copy of the pixels under them (`shader_blend`);
- **thousands of small batches** in the GS stream's order (native draws: `pcr_native.cpp`, `game/native_draw/`);
- **images in the GENERAL layout**, both frame buffers one image, no compression (`PC_OPTIMIZATION.md`).

A PC renderer would use the hardware depth test, a few dozen draws and compressed targets. The estimate is 1-2 ms on
the Vega 7, from bandwidth and triangle counts: **unmeasured**. This number decides how the rewrite starts: if the
world is cheap drawn plainly, start from a scene renderer; if not, find what else is in the way (textures, the CPU,
the driver) before rebuilding.

## The deliverable

A section in `fullscreen-layers-inventory.md` (or a new `docs/plans/world-plain.md`): the world's GPU ms a game frame
drawn the current way and drawn plainly, at a few scenes, 1x/2x/4x, in a real window with the owner's settings; what
each GS-exact mechanism costs; and what the floor is. Measurement and developer-only switches; nothing a player gets.

## Where to start

1. **The switches that exist** (each off the exact path, so the picture may differ; for timing only):
   `KF4_PC_DEPTH=gpu` (the GPU's interpolated depth instead of the depth rule; `=exact`, `=rule` to compare),
   `KF4_PC_SHADER_BLEND=0` (the nearest Vulkan blend instead of the shader's), `KF4_PC_DECAL_SETTLE=0`,
   `KF4_PC_STENCIL`, `KF4_PC_MERGED`, `KF4_PC_INDIRECT`, `KF4_PC_KNOCK` (leave a kind of work out of the pictures
   between: `n000`, `n280`, `2d`, `fills`, `clears`, `surface`). All `getenv("KF4_PC_` in `port/gs/pc/`; comments at
   each. Time each alone and together: that bounds what the exact mechanisms cost.
2. **Then a plain path**, if the switches leave doubt: the native draws (which already carry meshes, cameras and
   poses) drawn into a fresh image of their own with hardware depth, no shader blends, merged draws, an optimal
   layout; at least for one map's world. Its pictures need not match yet; its time is the floor.
3. **Measure** with `tools/layer_inventory.py --steps live` (real window, the owner's settings, 30 and 60, by shader
   state; add a scenario with `settings=` or `env=`), and `--profile-at` with `KF4_GPU_DETAIL=1` read by
   `tools/sampler/stacks.py --gpu` (`docs/port/PROFILING.md`). Per draw: `kf4-gsreplay --gpu-draws` (native draws off
   in a replay, so it times the GIF path; `tools/README.md`). What a full-screen pass costs on the card:
   `kf4-layerbench` (`PC_OPTIMIZATION.md`, "What a layer costs on this card").

## How to run things here

- Build: `cmake --build out/linux-clang`. Disc: `/home/voicedrew/Documents/King's Field - The Ancient City (USA).iso`
  (pass `--disc`). Tests: `ctest --test-dir out/linux-clang`.
- Timing in a **real window** with the owner's settings, not headless (`CLAUDE.md`, "Testing graphics");
  `layer_inventory.py`'s live step does it. On mains power; the owner timed the inventory on the balanced profile.
- Scenes: `layer_inventory.py --scenarios list` (099's start, the pond 009, the palace 022, combat at 002, under
  water); its base quick save starts a scene in about 13 s.
- Known leads to keep apart from this step (inventory, "Open questions"): native draw's DATE set-up (0.5-1.0 ms a game
  frame for 34 tiny draws), the repeated clear not recognised at most maps, the pump's surface copies (2.4-3 ms a
  picture between), the final step's separate passes (2.7 ms a picture).
- `decomp/` is read-only; game-side steering goes through `KF4_FX` (`game/screen_fx.c`).
- Commit on a branch; ask the owner before pushing.

# Volumetric fog (branch `feat/volumetric-fog`)

## Context
The game fogs per vertex, the PS2 way: each vertex gets `F = A + B/w` (A, B and the colour come from the map's fog blocks, faded per frame into `current_fog_block`), clamped to 0..255 and blended in `PCR_TEXTURE_AND_FOG` (`port/gs/pc/shaders/pcr_prim.h:644-674`). The result is flat, linear in 1/w, and shows steps on big triangles. The owner wants it replaced with volumetric fog: fog worked out per pixel from depth, with density that varies in space and moves.

Chosen design (my call, tell owner): a full-screen pass at the end of the world draw that marches each pixel's view ray through a fog volume. The fog keeps each map's own **colour and start/end distances**, so dungeons stay black-fogged and map 024 keeps its grey-then-black change. On top of that come height falloff (thicker low down) and slowly drifting 3D noise. Light shafts from torches are left for a later step: the game has no light list the renderer can use yet.
It is a setting, **Fog: PS2 / Volumetric**. The project's rule (`docs/port/PC_RENDERER.md:1270`) is that the PS2 look is the default and "real fog" is a choice. The default stays PS2 unless the owner says otherwise.

## Steps

1. **Fog values to the renderer.** Add a host hook next to `DrawDistance_*` (`port/include/port/host.h:231-243`, set in `patches/nonmatching.patch` where `func_00112BD0.c` fades `current_fog_block`). Each frame it gives the renderer A, B (after `DrawDistance_ScaleFog`) and FOGCOL. Put the maths in a small C unit in `game/` (`fog_volume.c/.h`, header written first): start `w0 = B/(255-A)`, end `w1 = -B/A`, the density that reaches the same fog at `w1`, and `w = az/(Z - cz)`. Add a unit test in `tests/` with an `add_test` in CMakeLists.txt.

2. **Camera to the scene builder.** `scn_build.cpp:530` now ignores `frame_mark`. Have it capture the world camera (view, sx, sy, cx, cy, cz, az: `frame_interp.c:93-120`). Queue the mark whenever volumetric fog is on, not only when pictures between are on. Store it in `scn::Scene` (`scn_scene.h:148-170`) with the fog values.

3. **Vertex fog off where the pass replaces it.** In volumetric mode, draws that write Z in the world drop `F_FGE` (`scn_build.cpp:433`, `:4962`). These keep vertex fog: draws that don't write Z (translucent effects, water), the sky (Z 0, and the pass skips Z 0), and the arm and held weapon (they use their own camera and are near). Shadows already have fog 0.

4. **The pass.** Add a new `scn::Op` / `PassKind` (`scn_scene.h:105-140`) recorded at `FrameInterp_EndWorld` (`patches/game/core.patch:1095`). That point is before the screen passes and HUD. Check that the arm is drawn after it; if not, keep the arm's depth out by its draw range.
   - Give the depth targets `SAMPLED_BIT` (`make_target_image`, `scn_build.cpp:3329-3344`). Player builds use D16 (Z/65535); exact Z uses D32F_S8, sampled through a depth-only view.
   - New shader `port/gs/scene/shaders/scn_fog_volume.frag`, added to `tools/build_shaders.py`, with `scn_shaders.h` regenerated. It rebuilds w from depth, marches 16 steps along the ray from `w0` to the pixel (or `w1`), and blends the map's colour by transmittance. The steps are jittered by a small blue-noise pattern to hide banding.
   - Pictures between: the op uses the picture's moved camera (`GsnInterpPicture.screen`, `frame_interp.c:179-210`), and noise time comes from the picture's time, so motion stays smooth at high rates.
   - Add its pipeline to the scene's ahead-of-time list (`scene-pipelines-listed`; check with `KF4_PC_WARM_LOG`).

5. **Setting.** Follow the anti-aliasing pattern (`3624d051`): a `port/include/port/fog.h` name table, `--fog ps2|volumetric` in `main_host.c`, `"fog"` in `port/overlay/settings.c`, live change via `PORT_LIVE_FOG` (`live_settings.h/.c`), and a row in `graphics_rows` (`port/ui/settings_rows.cpp`) next to Draw Distance. Use short, plain row text.

6. **Docs.** Add a section in `docs/port/PC_RENDERER.md` near the fog policy: what the pass does, what keeps vertex fog and why, and the cost.

## Verification
- `ctest --test-dir out/linux-clang` (new fog maths test included).
- `out/linux-clang-asan` (ASan+UBSan) run of a teleport at a fogged map with volumetric on.
- Pictures: headless `--dump-frames` with `--teleport` + `--invincible` at map 024 (grey fog), an outdoor map and a black-fog dungeon, in PS2 and volumetric modes. Check: no fog on the sky or HUD, the arm is not double-fogged, translucents look right, and fog fades with `current_fog_block`.
- Speed: real window run with the owner's settings (`KF4_READ_SETTINGS=1`, copied `KF4_DATA_DIR`, `tests/newgame.pad`, `--no-audio --saves none`) at 009 and 063. Compare GPU time with fog on and off using the frame profiler, and check the `[stats]` lines. Space the runs out.
- clang-tidy: none in the project (say so). Lint the patch lines only with `clang-tidy-diff` bugprone/analyzer.
- Commit on the branch; no push.

# The game's full-screen and screen-reading effects

Every effect the game draws over the whole screen, or draws from the screen's own picture, found in the
decompilation (`decomp/src/game`, the PC changes in `patches/game/`), with what turns it on, what it reads and
writes in GS memory, and how the PC renderers treat it: `port/platform/frame_interp.c` (the pictures between
at 60-240), native draw's passes (`game/native_draw/nd_passes.c`, `gsn_nd_pass.inc`) and `--renderer modern`'s
scene (`gsn_standalone.inc`, `gsn_composite.inc`, `gsn_scene_targets.inc`). The audit was made after the Queen's
Palace's purple wobble dropped the game to 14 frames a second at 240 (October 2026), to find the others before
play does. `tools/screen_fx_test.py` makes each one happen headless and measures it ("Tests" below);
`KF4_FX` (`game/screen_fx.c`, debugging only) forces the ones play reaches rarely.

## GS memory the effects use

| block (TBP/FBP x 32) | what | size |
|---|---|---|
| 0x0000, 0x1180 | the two frame buffers (`context1_buffer_half` picks the one drawn; the other is shown) | 640 x 448 PSMCT32 each (FBW 10), read as a 1024 x 512 texture (TW 10, TH 9) |
| 0x2BC0 | **shared**: the copy of the frame (`GsDraw_CopyFrame`, 640 x 448 PSMCT32, to 0x3D3F); the reflection's view (`Map_SetOffscreenTarget`, FRAME 0x0A04015E: 256 x 256 PSMCT16S, 0x2BC0-0x2DBF); the pause's and the map viewers' textures (`TextureTable_SetBasePointers(.., 0x2BC0, 0x3FE0)`) | |
| 0x3240 | the menus' work buffer (FBP 0x192): the shown frame copied there as the menu's backdrop, shown again after (`GameLoop_ShowGrabbedFrame`, TEX0 0x26802B240) | 640 x 448 PSMCT32 |
| 0x3540 | the Dark Succubus' body texture (`GsDraw_CopyFrameToTexture(0x3540, 0, 256, 256)`; inside the frame copy's range) | 256 x 256 PSMCT32 |
| 0x37E0 and others in 0x2BC0-0x3D3F | effect and model textures uploaded each frame (the frame copy overwrites them; `GameLoop_DrawFrame` re-uploads the effects' with `Effect_UploadTextures` after a copy) | |
| scrolling textures' targets | `MapScroll_UpdateDraw`/`UpdateCopy` (lava, water, the flesh cavern), `GsDraw_DrawScrollTexture` | 128 x 128 mostly |

## The effects

Order: as `GameLoop_DrawFrame` (`core/game_loop.c`) draws them after the world, then the ones drawn elsewhere.
"Reads the frame" means a texture whose TBP is the buffer being drawn (a feedback draw); "reads the shown
frame" the other buffer.

### 1. Under water: the copy, the wobbling screen, the tint

- **Trigger:** `PlayerState.status_bits` bit 39 (set by the collision code each frame the eye is below a water
  surface, cleared at the start of each played frame). Real: deep water (map 029 the Underground Canal:
  `bugs1024/quick_underwater`), and the deadly water's death screen (`bugs_modern/quick_deathwater`).
- **Draws:** `GsDraw_CopyFrame(0x2BC0)` (a local-to-local BITBLT, the frame drawn to 0x2BC0, 640 x 448 PSMCT32);
  `Game_DrawWobblingScreen(0x80, 0x80, 0x80, 0x80, 1.0)`: a 7 x 10 grid of 64 x 64 cells whose corners move as
  springs (`wobble_grid_points`, rand), 70 textured quads (GIF packets, TEX0 0x26802ABC0 = TBP 0x2BC0 TBW 10
  PSMCT32 1024 x 512, TEX1 0: nearest, no mipmaps, ALPHA 0x44, Z writes off) over the screen; then
  `Effect_UploadTextures` (the copy overwrote them); after the arm `Game_TintScreen(0x5E, 0x87, 0x92)`: the frame
  over itself in ten 64-pixel strips, modulated (reads the frame, TEX0 TBP = the frame drawn, TFX modulate).
  **On map 035** (the Guardian's Room) before its water is purified (`guardians_room_water_purified`):
  `GsDraw_FillScreen(0x40, 0x80, 0x40, 0x78, 0x44)` (a green fill) instead of the tint.
- **frame_interp:** the copy is one of the frame's copies (`Uploads.copies`); `flat_copies` makes it again in
  each picture between from that picture's own frame when the frame reads it after the copy and not before
  (the wobble does); the death-water fix (ce05fff9) counts a mipmapped texture's reads over its real levels. The
  wobble's quads are 2D items paired with the frame before's (`flat_pair`), moved t of the way. The tint is a
  pass reading the frame (TEX0 moved to the picture's buffer by `ad_write`).
- **Native draw / modern:** the tint is `NativeDraw_TintScreen` (a screen pass); the copy and the wobble stay
  the PS2 path's packets. The scene gives the GS the frame at the copy and keeps its own picture as copied
  (`sa_give(true)`, `sa.fcopy`), read as the texture at 0x2BC0 until those pages are written. A GPU copy of
  the frame over whole pages drops the targets wholly under it without writing them back (`overwrite_lo`,
  perf240).

### 2. The MP drain (the Queen's Palace, the Giant Leader): the purple wobbling screen

- **Trigger:** `PlayerState.mp_drain_tint` 1-8: `Player_Update` raises it each frame `pending_mp_drain` is set
  (`Attack_DrainPlayerMp`: the Watchers of map 022 the Queen's Palace, `enemy/watcher.c`; the Giant Leader of map
  034, `enemy/giant_leader.c`) and lowers it by one a frame after.
- **Draws:** the same copy to 0x2BC0 and `Game_DrawWobblingScreen(r, g, b, 0x80, tint)` with
  r = 128 - 58t, g = 128 - 78t, b = 128 + 12t (t = mp_drain_tint / 8): purple, the wobble scaled by t. With
  under water at once, two copies and two wobbles.
- **PC:** as under water. **At 240 in modern it dropped the game to 14 frames a second** (the exact restore drew
  the whole frame again for the scrolling textures, and the wobble read the copy from VRAM: fixed on
  `p8/queen`, NATIVE_DRAW_P8.md "The Queen's Palace: the MP drain at 240"); at a reflecting pool, a read-back a
  frame more ("Fixes" 2-4 below).

### 3. Player_DrawScreenFade, the haze, the debug depth sprite

- `Player_DrawScreenFade`: `PlayerState.screen_fade` > 0 a white fill (ALPHA 0x48), < 0 black (0x44), Z writes
  off. No read. (Set by `Player_UpdateScreenFade`: spells and scripted flashes.)
- `Game_DrawHazeLayers` (MapDrawState bit 44): a full-screen sprite of the frame over itself at each haze
  layer's depth (`haze_layer_depths`, projected once at boot), ALPHA 0x80000000A8, TEST 0x51001 (Z test), CLAMP
  5: reads the frame. **No code of the game sets bit 44** (only `Map_ResetGameState`'s defaults: bits 42, 45,
  46): unreachable in play; `NativeDraw_HazeLayers` exists. `Game_DrawHazeLayersShifted`: never called.
- `Game_DrawDepthTestSprite` (bit 41): a debug sprite, pad 1's L1/L2 move its depth. Never set by the game.

### 4. The one-row blends (bits 42 and 43): the flicker filter

- `Game_BlendScreenOffsetDown` (bit 42, **set at every map reset**): the frame over itself one pixel lower at
  half strength, ten 64-pixel strips, TEXFLUSH first, Z writes off: reads the frame. The PC turns it off by
  default (`--flicker-filter` keeps it; `NativeDraw_FlickerFilter(0)`). Also drawn by the King's Map
  (`MapViewer_DrawKingsMap`).
- `Game_BlendScreenOffsetUpDown` (bit 43): down then up. Never set by the game (`NativeDraw_FlickerFilter(1)`).

### 5. The frame before (bit 40, the knock-back)

- `Game_BlendPreviousFrame(alpha)`: the shown buffer (the frame before) over the whole screen at alpha, one
  sprite, ALPHA 0x44, TEST 0x51001: **reads the shown frame**. Drawn for MapDrawState bit 40
  (`previous_frame_alpha`, 0x40 by default; never set by the game) and **while `PlayerState.aim_speed` > 0**:
  the knock-back of a hit (`Player_KnockBackFromHit`, `Player_KnockBack`), every fight.
  `Game_BlendPreviousFrameZoomed`: never called.
- **frame_interp:** a picture between reads the native GS's copy of the frame shown, by a^t (`replay_pending`,
  `move_flat_vertex`); native draw's pass says so with `RD_ND_READS_SHOWN`. **Modern:** reads the GS's other
  buffer (at the GS's scale).

### 6. The view-turn shift (bit 45)

- `Game_ShiftScreenByViewTurn` (bit 45, set at every map reset; the main loop's draw only): the frame drawn again
  moved by the view's turn since the frame before (at most 8 pixels), over every other line (SCANMSK 3), 20
  strips: reads the frame. A picture for the other field of an interlaced TV. The PC skips it with whole frames
  (the default; `--no-whole-frame` keeps it). `Game_ShiftScreenByFocusTurn`: never called.

### 7. Screen tints and fills (no reads)

All `GsDraw_FillScreen` (an untextured sprite over the screen, ALPHA 0x44 or 0x48) or the same through
`NativeDraw_FillScreen`; in modern the composite's chain of "ops over the screen" (`gsn_composite.inc`).
- `Player_DrawScreenTints` (`player_screen_tint`, set by `Player_UpdateScreenTints`): green (kind 0), dark
  yellow (1), purple (3), white (4), a cycling colour (5), darkness (subtracted), the red damage flash
  (`Player_FlashDamage`, dark red while purple shows), the death fade (black). The status ailments and the
  light spells: no screen reads.
- `Map_CoverScreenWithMapTint` (draw mode 1, between the mirror view and the world: the map header's colour).
- MapDrawState's screen fill (script command 197, `ScriptCmd_SetScreenColor`).
- `Game_DrawFade` (the scripted fades), `Splash_Draw`'s darkening, the message dim (`message_dim_alpha`), the
  pause's shade (START: `game_status.paused`).

### 8. The brightness option (every frame)

- `Game_ApplyBrightness(game_options[10])`: the frame over itself in ten strips, modulated by brightness / 128,
  last of every frame (over the HUD): reads the frame. `NativeDraw_Brightness`; modern's composite chain.

### 9. The Dark Succubus: a model drawn with the screen's picture

- **Trigger:** an entity whose spawn record's chr record is 0x47 (the Dark Succubus, map 038 the Dark Sanctuary,
  once the story has woken it): `EntityPool` gives its model `MODEL_FLAG_800` and the draw hook
  `Entity_PrepareScreenTexturedDraw`.
- **Draws (inside the world, before the model):** `Model_SetUvsFromScreen` (each vertex's UV from where it
  lands on the screen), `GsDraw_CopyFrameToTexture(0x3540, 0, 256, 256)`: a draw into a 256 x 256 PSMCT32 target
  at 0x3540 of one sprite reading the frame drawn (TEX0 TBP = the frame, 1024 x 512, TCC 1, ALPHA
  0x80000000A8, TEST 0x51001, CLAMP 5), then `GsDraw_ResetDrawTarget` and TEXFLUSH; the model then reads 0x3540
  (TEX0 0x220013540). The screen as it is up to that model, refracted through its body. Its parts are drawn in
  both contexts: context 1 with ALPHA 0x44 (As 0x80 from the vertices, TCC off: the copy's colour as it is) and
  Z tested, context 2 with TEST_2 0x30001 (alpha test NEVER, fail KEEP: it writes nothing).
- **frame_interp:** a draw into another buffer from the frame (`Uploads.copies`, "the screen's picture on a
  model"), made again in a picture between under the same rule as the copy. **Modern:** a draw into a target
  other than the scene's reading the frame: the scene gives the GS the frame there.
- **The real fight** (October 2026, the owner's `bugs1026/quick_succubus_hotspot`: map 038 at the room's
  entrance, the camera on her): the F3 counter read `57 FPS / 15 GAME`. Not the copy (one a frame: no write-back,
  no decode) but her body's two passes in the native GS (it reads a render target, so modern's scene leaves it
  to the GS): the first blended by the shader (a draw whose texture is a target, `(Cs - Cd) As + Cd`), in runs
  of triangles that do not overlap, with MSAA each a render pass and a resolve of the whole frame; the second,
  which writes nothing, a render pass without the Z buffer between each two parts. 176 render passes and 2.6 GB
  loaded a picture in modern, 336 and 8.9 GB hosted: bound by the GPU, the game's thread spinning in
  `port_dmac_wait` for the GS's. Fixed on `perf/succubus` (NATIVE_DRAW_P8.md, "The Dark Succubus' body"): 5
  render passes a picture, the game at 30.
- `KF4_FX=copytex` draws the same copy and a 256 x 256 sprite from it after the world (the same GS work, not in
  the world); `KF4_FX=succubus` puts the player before her (with `--invincible`, her body over much of the screen).
- **Test:** the `succubus` case (standing still from `quick_succubus_hotspot`) and `succubus near` (`KF4_FX=succubus`).

### 10. The reflecting pools (the reflection's view, draw mode 2)

- **Trigger:** maps with mirror water (mode 2, e.g. 009's pond: `bugs1024/quick_pond`).
- **Draws:** the world again from below into 0x2BC0 (256 x 256 PSMCT16S, `Map_SetOffscreenTarget`), then the
  water's map records (flag 0x200) with `Wobble_DrawRippledModel`: their vertices' UVs from their screen
  places moved by `wobble_sines`, reading 0x2BC0 (TEX0 0x220A12BC0). P6 (`nd_water.c`); modern: a named
  render target (T3), the reflection kept exactly when the camera is still (pond240).
- **The same pages as the frame copy:** with the MP drain or under water at a pool, the copy of the frame
  overwrites the reflection's target in the same frame.

### 11. The King's Map and the picture maps

- `MapViewer_RunKingsMap` (item 5; not usable on maps 000, 002, 004, 008, 009, 098, 099): its own loop
  (`Display_SwapBuffers`), the map's records drawn from above, then `Game_BlendScreenOffsetDown` and
  `Game_TintScreen(0xEA, 0xB2, 0x46)` (both read the frame), its panel textures at 0x2BC0. The picture maps
  (items 1-4, `MapViewer_RunPictureMap`): 2D pictures. Before a viewer, `GameLoop_ShowGrabbedFrame` or
  `GameLoop_RedrawFrame`. No pictures between (a loop of its own).

### 12. The menus' backdrops

- `Menu_CopyFrameToTexture` (CIRCLE: the game's menu), `Trade_CopyFrameToTexture`, `Smith_CopyFrameToTexture`,
  `SaveScreen_DrawBackground`: the shown buffer drawn into the work buffer (FRAME FBP 0x192, 0x3240) and back
  into the frame at half brightness (feedback through another target); after the menu
  `GameLoop_ShowGrabbedFrame` draws 0x3240 into both buffers. Their own loops, 60 a second, no pictures between.
  Modern: the scene gives the GS the frame ("the menus' backdrop", `sa_give`).

### 13. The map change

- `MapChange_CaptureFrame` (`MapChange_RunToEnd`: a door into another map, script command 174, and a wand used
  on a Symbol of Passage, `MapChange_StartPassage`): one frame drawn and `GsDraw_CopyFrame(0x2BC0)`; then
  `MapChange_FadeOutCapture` draws 0x2BC0 (TEX0 0x26802ABC0) in ten strips each frame while the map loads (its
  own loop, no pictures between), then black, then the new map's fade in.
- **frame_interp:** the frame does not read the copy after it, so the pictures between do not make it again;
  but they send the frame's uploads again, and those went into 0x2BC0-0x3D3F before the copy. **At 60-240 the
  fade showed those textures for about a second** (every renderer, paraLLEl-GS too; never at 30: the owner's
  Green Wand, October 2026). Such a copy is now `COPY_RESTORE` (`flat_copies`: not read after, over pages
  written before it, the frame not drawn into after it): the exact restore after the vblank's pictures
  (`port_interp_end_vblank`) sends the uploads again and then the copy, from the game's frame. The fade's
  frames are those at 30 byte for byte (NATIVE_DRAW_P8.md, "The wand's passage").
- **Test:** the `wand` case (`bugs1025/quick_wand`, standing on a Symbol of Passage: the menu, Item, Up x4 to
  the Green Wand, Cross).

### 14. Other copies and targets

- The scrolling textures (`MapScroll_UpdateDraw`/`UpdateCopy`, `GsDraw_DrawScrollTexture`): 128 x 128 targets the
  map's records read (lava, water, the flesh cavern's walls), drawn each frame; frame_interp draws them again in
  each picture between from the same packets and `port_interp_end_vblank` restores them exactly. The wave UVs
  (`Wobble_ApplyWaveUvs`, the flesh cavern 039/040/042) move texture coordinates only.
- Debug only: `ScreenDump_*` (local-to-host transfers of a buffer), `debug.c`'s fills.

### 15. Not a screen effect: the icy corridor's creatures (map 017)

- **The owner's report** (October 2026, `bugs1025/quick_icy`): an icy stone corridor, the picture washed grey,
  `56 FPS / 11 GAME`. The grey is the map's own fog and fill: no screen effect is on there (no copy, no wobble, no
  haze: bit 44 stays off, as above), and the white tint was on in the areas before without a slowdown.
- **What costs:** the creatures in the corridor. They are skinned models (VU1 program 0x280) whose every part is
  two passes: the first (ALPHA 0x44, As 0x80 everywhere: opaque, Z written) and the sphere-mapped shine (ALPHA 0x9,
  Cs + Cd As, with As up to 146). An As above 0x80 is more than Vulkan's blending can do, so the shine is blended in
  the shader, in runs of triangles that do not overlap (each run a copy of the pixels it blends over, two barriers,
  its draws): about 700 parts and 2,400 runs a picture, 5,700 Vulkan draws and as many pipeline binds a picture
  between. Nothing is read back or decoded.
- **PC:** the shine's Z-first draws are left out (perf/icy, NATIVE_DRAW_P8.md "The icy corridor"). Modern draws the
  shine whole by default (perf/shine-whole, NATIVE_DRAW_P8.md "The ice shine drawn whole"): where a part folds over its
  own edge the GS blends both triangles there, drawn whole one is (about 100 pixels a 3x picture, main's brighter by up
  to 64 levels); the skinned groups then take the parts, one copy a group of parts (with MSAA too, the default: drawn
  part by part, MSAA drew a bright dashed line along the parts' seams; `KF4_SA_SHINE_GROUP=1` restores that). `--native-shine exact`
  (`settings.json` `display.native_shine`, the overlay's "Ice shine: exact PS2 folds (slower)") keeps the runs: main's
  picture. Hosted (`--native-draw on`) is slower there: each run is a
  render pass and an MSAA resolve.
- **Test:** the `icy` case (standing still from `quick_icy`, the shine whole) and `icy exact` (`--native-shine exact`).

## Tests

`tools/screen_fx_test.py` runs each effect as a case (`--cases list`): the forced ones from a quick save in play
(`--save`; without it a new game) with `KF4_FX` (`game/screen_fx.c`: the passes' state set for the draw only and
put back after, so the game goes on as without it), some after a `--teleport` (035 for the poisoned water, 029 for
the King's Map) or with pad presses (START: the pause; CIRCLE: the menu; TRIANGLE every 120 vblanks with
`KF4_FX=spell=N` and `--cheats spells,magic`), and the real scenes from quick saves in `--spots`
(`quick_underwater`, `quick_deathwater`, `quick_palace`, `quick_wand`, `quick_icy`, `quick_succubus_hotspot`). Each at the owner's settings (240, 3x, native
widescreen, MSAA 2, aniso 2, cut-out AA ssaa, `--renderer modern`), always `--no-audio` and headless:

- `--perf`: real time (`KF4_PRESENT_SIM=240`, `KF4_GSN_GPU_TIME=2`, `KF4_HITCH_MS=50`): pictures and game frames a
  second over the case's vblanks (`KF4_PRESENT_LOG`), write-backs and decodes a picture, pictures between drawn
  from the packets, the fallback's cuts, hitches, GPU time a picture. Only meaningful on an idle PC.
- `--cost`: `--fast` with every picture between drawn (`KF4_INTERP_DRAW_ALL=1`): the GS thread's time a game
  frame, a picture between from the packets and from the recording, write-backs and decodes in all. CPU work
  only, so it holds up with the GPU shared (the owner playing).
- `--pictures`: every picture of 12 vblanks dumped (`--fast`) and checked with `flash_find.py --frames` (a
  picture between not between its game frames is flagged: the death-water and under-water bugs were of this
  kind). The `wand` case also runs at 30 and wants its game frames there (vblanks 740-749, the map change's fade:
  a loop of its own, every vblank a game frame, nothing for flash_find to judge) byte for byte the 240 run's
  (`same30`; before the fix 8 of 10 differed).

`KF4_FX` names: water, drain, drainramp, prev, aim, haze, depth, down, updown, shift (with `--no-whole-frame`),
fill, fade, tint, dim, bright40, brightff, copytex (the Dark Succubus' screen copy, drawn after the world),
succubus (the player put before it, map 038, once the story has woken it), mapview=N, spell=N; `KF4_FX_LOG=1`.

## Results (2026-10-24, from the pond save `bugs1024/quick_pond`: the reflection's 0x2BC0 under the copy)

Real time at 240, before the fixes below (native/draw 6234768a, the PC idle; the base reached 239.5 pictures a
second). The fills, tints and passes that do not read the frame came out between 60 and 240 in that run
because another game started on the PC part way through it; re-run back to back (base, fade, base, fade) they
were the same as the base (140/139, 166/172), and `--cost` finds nothing in them.

| effect | pictures/s | game fps | write-backs a picture | decodes a picture | verdict then |
|---|---|---|---|---|---|
| base (the pond) | 239.5 | 29.9 | 0 | 0 | OK |
| under water, forced (`water`) | 59.7 | 30.0 | 0.77 | 0.01 | slow |
| MP drain, forced (`drain`, `drainramp`) | 58.6-59.1 | 30.0 | 0.77 | 0.01 | slow |
| both (`water+drain`) | 38.6 | 30.0 | 0.78 | 0.37 | slow |
| the frame before (`prev`), the knock-back (`aim`) | 15.0 | **11.5-11.8** | 0.98 | 0.41 | slow |
| the Dark Succubus' screen copy (`copytex`) | 34.8 | 27.0 | 0.82 | 0.14 | slow (the case's own TEX0 bug; fixed: OK) |
| the death screen in deadly water (`deathwater`, real) | 147.4 | **23.6** | 0.05 | 0.05 | slow (2 frame decodes a frame, 60 ms) |
| under water (real, map 029) | 137-202 | 29.9 | 0.11 | 0.03 | OK |
| the Queen's Palace (real) | 56.5 | 29.8 | 0.29 | 0.06 | fixed by p8/queen (8fe70d12) |
| haze, depth sprite, one-row blends, view-turn shift | 167-234 | 29.9 | 0 | 0 | OK |
| fills, fades, tints, dim, pause, brightness 0x40 / 0xFF | | 30 | 0 | 0 | OK (see above) |
| King's Map, menu, pause, poisoned water (035) | | 30 | 0-0.18 | 0-0.05 | OK (menus and the map viewer have no pictures between: 60) |
| 14 spells (3 ... 81) | 89-186 | 30 | 0 | 0 | OK (the pond is GPU-heavy; spells 3 and 81 flagged, below) |

The GS thread a game frame (`--cost`, every picture between drawn; ms), native/draw 8fe70d12 (the Queen's fix in)
against the fixes below:

| effect | GS a frame | a picture between from the packets | write-backs (in all) | decodes (in all) |
|---|---|---|---|---|
| base | 22.7 -> 20.8 | 7.4 -> 6.5 | 0 -> 0 | 14 -> 11 |
| under water, forced | 32.4 -> 26.5 | **39.0 -> 10.0** | **438 -> 0** | 14 -> 11 |
| MP drain, forced | 43.3 -> 25.3 | **49.6 -> 9.5** | **438 -> 0** | 14 -> 11 |
| both | 42.3 -> 27.2 | 48.2 -> 10.3 | 438 -> 0 | 14 -> 11 |
| the frame before | **55.0 -> 21.2** | 7.2 -> 6.6 | **1,095 -> 0** | **452 -> 11** |
| the knock-back | **44.5 -> 20.9** | 6.9 -> 6.5 | 1,095 -> 0 | 452 -> 11 |
| Dark Succubus copy | 22.3 -> 22.1 | 8.3 -> 8.3 | 0 | 14 -> 11 |
| under water (real) | 8.1 -> 9.1 | 3.8 -> 4.3 | 0 | 16 |
| death water (real) | 6.7 -> 7.7 | 2.7 -> 3.0 | 0 | 19 |
| Queen's Palace (real) | 20.3 -> 22.8 | 8.4 -> 9.5 | 0 | 15 |

(The small rises in the real scenes are noise: the owner was playing on the same PC.) Every case's pictures
between: 0 flagged, but spells 3 and 81 (12 and 25 of 48, at most 1.9% of the picture): the bursts' own
animation (wisps and flames redrawn each frame, decision 5 of NATIVE_DRAW_TASKS.md), not broken pictures.
Haze (`haze`) costs 13.6 ms a picture between from the packets against 6.6: no code of the game turns it on.

## Fixes (branch `p8/screen-fx`)

1. **The frame before read from its target** (`gsn_standalone.inc`, `sa_texture`'s `read_extent`):
   `Game_BlendPreviousFrame`'s sprite has its corners half a texel above the top; the scene took that as reading
   all 512 rows, missed the shown frame's 640 x 448 target and decoded the frame from VRAM (written back first)
   every frame. Rows now as `flush_batch`'s `rows_read` counts them. The knock-back of every hit and the death
   screen's: 12 -> 30 game frames a second.
2. **A named target a transfer writes whole is not given back first** (`gsn_scene_targets.inc`, `sat_transfer`):
   the copy of the frame to 0x2BC0 covers the reflection's 256 x 256; given back each frame, the GS's target was
   made from VRAM, which first took the last copy back from the GPU.
3. **A batch wholly outside its scissor makes no target** (`gsn.cpp`, `flush_batch`): the reflection's map strips
   below its 256 x 256 view made the GS's target at 0x2BC0 from VRAM each frame, with the same read-back.
4. **A picture between's own target is kept over an upload only while a picture between is drawn** (`gsn.cpp`,
   `prepare_write`): the copy of a picture's frame left at 0x2BC0 had every upload of the game's frame over it
   loaded into it again, page by page (about 540 pages a frame under water at the pond).

All four keep the pictures byte for byte: the four spots (pond, fortress, the canyon, under water: 156 pictures
at 240, every phase), the herb run in modern at the owner's settings (the same `--log`, PATH1 hash
561c91020b2908ee), and the forced water, drain and screen copy at the pond (40 pictures each). The off check
passes: boot_test; frame_test `--renderer gs` and `native`; gsn_draw_test 0 of 300 (seeds 1, 7, 1234); vu1_test 0
of 90; herb with `--native-draw off` against play-932801f: the same `--log`, PATH1 hash 204e443a9fc49731, VU1
cycles 460424273, pictures 1800, 1820 and 1840 byte for byte.

**Left:** a door's map change (the wand's passage is the same capture and fade, tested: `wand`); the haze's cost (unreachable); real-time numbers after the fixes on an idle PC
(`python tools/screen_fx_test.py --perf --save .../quick_pond.kf4quick --spots ...`).

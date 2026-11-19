# The full-screen layers: an inventory (step 1)

**2026-11-19, later: the savings below are to be built into the scene renderer** (its final pass), not into today's
renderer (`renderer-scene-not-gs.md`, "The decision").

2026-11-19. Step "Order 1" of `renderer-scene-not-gs.md`: every full-screen layer King's Field IV draws, what it is in
the game's code, what it costs on the GPU, what reads it, and whether a fast clear, the removal of dead writes or a fold
into the scanout could take it away exactly. Measurement only: the renderer draws as on `main` (8ebebb84).

Made by `tools/layer_inventory.py` (tools/README.md, "layer_inventory.py"), which can be run again after each step of the
redesign. Its raw output (captures, pictures, profiles) is the game's and stays outside the repository.

## The answer, short

At the owner's settings (2x with native widescreen) on the Vega 7, a game frame of play costs a median **12.8 ms** of
GPU in the tour's scenarios (9.9 ms at the 64 maps' starts; replays, native draw off). The full-screen layers are a
median **3.3 ms of it (27%)**, up to 53% under water. They are the same few everywhere:

| Every frame, every map | What it is | Class | ms a game frame | In a picture between |
|---|---|---|---|---|
| The swap's first clear | `sceGsSwapDBuff` for `game_dbuff` | (b) dead | 0.46 | left out already |
| The swap's second clear | the same for context 2's double buffer | (a) clear | 0.47 (0.002 where recognised) | drawn |
| `GsDraw_ClearDepth` | Z 0 before the arm | (b) dead in almost every frame | 0.19 | left out already |
| `GsDraw_SetScreenAlpha(0)` | the alpha DATE reads, set | (e) alpha only | 0.42 | drawn |
| `Game_ApplyBrightness` | the frame times 136/128, last | (c) foldable | 1.24 (its copy of the frame in it) | drawn |
| MapDrawState's screen fill (not every map) | black at alpha 2: 099's script, from a New Game | (c) foldable | 0.54 | drawn |

What each change would save a game frame (median over play, the range in brackets):

| Change | A game frame | A picture between | Blocked by |
|---|---|---|---|
| Fast clears (each buffer an image of its own) | 0.47 (0.33-0.68) | the same | nothing in play |
| Dead writes left out of kf4-pc's frame | 0.66 (0.45-1.11) | nothing: the pump does it already | nothing |
| Folding into the scanout | 1.75 (0.52-2.15) | the same | the frame before blended in (the knock-back) keeps the last picture; tints under the HUD fold at their place only |
| The alpha's reset as a stencil clear | 0.42 | the same | the alpha copied to 0x2BC0 under water |
| Still drawn (d and e) | 0.43 median, 2-3.4 under water and in the MP drain, 5.4 with the (unreachable) haze | | |

Not layers by the rules here, but full-screen costs of the same size that the inventory found ("Full-screen costs
that are not layers", "The real window"): the pump's **copies of the frame's surfaces** for its pictures between (2.4-3
ms of every picture between: as much as all the layers), native draw's **DATE** set-up for the entity pool's models
(0.5-1.0 ms a game frame and as much a picture between where the pool draws, most scenes; 34 tiny draws at 099), **the sky** read from a scrolling texture's
target (0.8 ms a game frame), and **the scanout and present** every vblank (5.4 ms a game frame at 30).

In the real window a game frame costs 15-21 ms of GPU in play and **a picture between 16-28 ms, as much or more**: the
pump draws every layer again (but the dead ones) and copies the frame's surfaces first.

**For step 2:** folding first. It is the largest item (the brightness pass and the fills after the HUD, 1.2-1.8 ms of
every game frame and of every picture between), it needs no change to how GS memory is laid out (the brightness pass
reads its own pixel, the fills read nothing), and its one blocker is a history image of the last picture, which a PC
renderer wants anyway. Leaving out the dead writes in kf4-pc's frame goes with it (the pump's `pcr_dead_writes.h`
logic, 0.66). Separate targets come after: the clears alone are worth 0.47, and what compression would give the
world's draws is not something this inventory measures. The memory the targets would have to untangle is below
("Targets and the GS memory they share"). Two non-layer costs are as large and stand on their own: the pump's copies
(2.4-3 ms a picture between; `renderer-scene-not-gs.md`'s step 3) and native draw's DATE (0.5-1.0 ms a game frame and
as much a picture between, a fix in the native batch's DATE set-up).

## The scenarios

Every scenario was captured and replayed; those with a live moment were also run in the real window at 60 and 30
(the scenes without input of their own from a quick save taken in 099 after a New Game: the same scene 25 s sooner,
but without the screen fill 099's script sets on entering the map). "Forced": the effect is made by `KF4_FX` or an
option, not reached by play.

| Scenario | What it shows | Forced | Captures (vblanks) | Live |
|---|---|---|---|---|
| title | the logos, the title, Press Start, New Game, the opening movie's end, the fade into play |  | 300, 420, 520, 760, 1450 |  |
| start | New Game's first map (099), standing |  | 1800 | yes |
| start native off | 099 standing, the owner's settings but native draw off (the GS stream's passes and DATE drawn as GIF batches): against "start", what native draw's passes cost |  | 1800 | yes |
| start spin | 099, a mouse spin (KF4_DIAG_SPIN=6: through instant mouse look, as a mouse) |  | 1800 | yes |
| walk | 099, walking, a stick turn, looking down and up |  | 1620, 1780, 1915 | yes |
| pickup | 099, walking to the herb, picking it up, its message box and the message dim |  | 1600, 1832, 1860, 1930 | yes |
| pause | 099, the pause (START): the shade and the paused HUD |  | 1900 | yes |
| menus | every page of the menu (tests/motion/menus.pad): its backdrop, the grab, the pages |  | 1805, 1820, 1910, 2450, 2840, 3500, 3725, 3760 | yes |
| bright40 | 099, the brightness option at 0x40 | yes | 1800 | yes |
| brightff | 099, the brightness option at 0xFF | yes | 1800 | yes |
| flicker | 099, --flicker-filter (Game_BlendScreenOffsetDown, MapDrawState bit 42) |  | 1800 | yes |
| fx water | 099, KF4_FX=water: under water: the frame's copy to 0x2BC0, the wobbling screen, the tint | yes | 1800 | yes |
| fx drain | 099, KF4_FX=drain: the MP drain's purple wobbling screen, full strength | yes | 1800 | yes |
| fx prev | 099, KF4_FX=prev: the frame before blended in (MapDrawState bit 40) | yes | 1800 | yes |
| fx aim | 099, KF4_FX=aim: the knock-back's frame before (aim_speed) | yes | 1800 | yes |
| fx haze | 099, KF4_FX=haze: the haze layers (bit 44; no code of the game sets it) | yes | 1800 | yes |
| fx depth | 099, KF4_FX=depth: the debug depth sprite (bit 41) | yes | 1800 | yes |
| fx updown | 099, KF4_FX=updown: the one-row blends down and up (bit 43) | yes | 1800 | yes |
| fx shift | 099, KF4_FX=shift: the view-turn shift (bit 45, --no-whole-frame) | yes | 1800 | yes |
| fx fill | 099, KF4_FX=fill: MapDrawState's screen fill (red, 0x40) | yes | 1800 | yes |
| fx fade | 099, KF4_FX=fade: PlayerState.screen_fade (white, 0x40) | yes | 1800 | yes |
| fx tint | 099, KF4_FX=tint: player_screen_tint's purple and darkness | yes | 1800 | yes |
| fx dim | 099, KF4_FX=dim: the message dim | yes | 1800 | yes |
| fx copytex | 099, KF4_FX=copytex: the Dark Succubus' screen copy into 0x3540 and a sprite from it | yes | 1800 | yes |
| pond | 009, the swamp: the reflection's view into 0x2BC0 with its Z in the frame's Z buffer |  | 2000, 2100 | yes |
| forest | 014, the Heart of the Forest: the reflection drawn 256 wide over the frame's Z |  | 2000, 2100 | yes |
| canal water | 029, the Underground Canal, under water forced (KF4_FX=water) | yes | 2000 | yes |
| guardian water | 035, the Guardian's Room, under water forced: the green fill instead of the tint | yes | 2000 | yes |
| palace | 022, the Queen's Palace, before a Watcher (its MP drain: the purple wobble, when it drains) |  | 2100, 2300, 2500 | yes |
| giant leader | 034, the MP drain forced (KF4_FX=drainramp: its colour and wobble changing) | yes | 2000, 2016 | yes |
| succubus | 038, the Dark Sanctuary, KF4_FX=copytex (the Dark Succubus' screen copy; she sleeps in a new game) | yes | 2000 | yes |
| flesh | 039, the flesh cavern: the wave UVs and scrolling textures |  | 2000 | yes |
| icy | 017, the icy corridor |  | 2000 | yes |
| salamanders | 016, before the salamanders (their fire) |  | 2100, 2300 | yes |
| fire elementals | 102, before the fire elementals |  | 2100, 2300 | yes |
| fire genie | 025, before the fire genie |  | 2100, 2300 | yes |
| combat | 002, the club put on, a Maneater before the player: swings (the weapon's trail), its hits, the red flash, the knock-back |  | 2210, 2295, 2510, 2650, 2810 | yes |
| first kill | 002, a Fireball kills the Maneater: its death and the reward |  | 2440, 2600, 2645 | yes |
| death | 002, a Maneater before the player, not invincible: hits until the player's death (vblank 3826), the death fade and the game over screen |  | 3600, 3860, 3920, 3980 |  |
| spell 3 | 099, spell 3 cast (KF4_FX=spell=3, tests/motion/cast.pad) |  | 1905, 1925, 2030 | yes |
| spell 16 | 099, spell 16 cast (KF4_FX=spell=16, tests/motion/cast.pad) |  | 1905, 1925, 2030 | yes |
| spell 36 | 099, spell 36 cast (KF4_FX=spell=36, tests/motion/cast.pad) |  | 1905, 1925, 2030 |  |
| spell 61 | 099, spell 61 cast (KF4_FX=spell=61, tests/motion/cast.pad) |  | 1905, 1925, 2030 |  |
| spell 76 | 099, spell 76 cast (KF4_FX=spell=76, tests/motion/cast.pad) |  | 1905, 1925, 2030 |  |
| spell 81 | 099, spell 81 cast (KF4_FX=spell=81, tests/motion/cast.pad) |  | 1905, 1925, 2030 |  |
| mapview 1 | the map viewer of map item 1 | yes | 1900, 1990 |  |
| mapview 5 | the map viewer of map item 5 (the King's Map, at 029) | yes | 2100, 2190 |  |
| map change | 099 to 002 by the game's map change (KF4_FX=mapchange=2, as a door with no link): the frame captured to 0x2BC0, its fade (its own loop), the new map's fade in | yes | 1796, 1799, 1802, 1806, 1830, 1880 |  |
| save point | 099, the save screen as a save point opens it (KF4_FX=savepoint): its backdrop, its pages | yes | 1700, 1730, 1760, 1800, 1840, 1900 |  |
| teleport | 099 to 002 by --teleport (a quick load: the load's fade, not the map change's) |  | 1802, 1812, 1840 |  |
| map NNN (64) | each map's start from a made-up quick save, a walk and a turn | | 260, 360 | |

Asked for and not reached, or reached another way: the deadly water's death screen (the player's death by a Maneater was reached instead: the death fade is `Player_DrawScreenTints`' black fill under the HUD, 0x28 to 0x68, and the game over screen a loop of its own, five draws a frame; no deep-water death was set up); a door to another map (the same `MapChange_RunToEnd` as `KF4_FX=mapchange`); a real save point (`KF4_FX=savepoint`, the same `SaveScreen_Run`); the Green Wand's passage (the same capture and fade as the map change); opening a door (a model moving, no full-screen layer); the one-row blends up and down (`KF4_FX=updown` draws nothing without `--flicker-filter`, which the scenario did not pass; `flicker` measures that kind of pass); 1x and 4x were replayed for five scenarios ("1x, 2x and 4x" below), not run in the real window. Spells 36 and 61 drew no layer of their own in their windows; 76 drew fills (0x04-0x50) and 81 the red flash.

## How it was measured

- **The machine:** the Ryzen 5 5625U laptop (Vega 7 integrated graphics, RADV, Mesa 26.0.8), on mains power with the
  **balanced** platform profile (the owner's choice for this run; `PC_OPTIMIZATION.md` measured 5-20% moves in the
  GPU's and memory's clocks with the profile and the battery, so compare ratios across runs, not single ms). The 9070
  XT was not run.
- **The settings:** the owner's: 2x with native widescreen (1920x896 a buffer, both buffers one 1920x1856 image), FXAA,
  16x anisotropic, `native_draw` on, instant mouse look on, brightness 136 (a new game's).
- **The scenarios** (below): a New Game and pad scripts (`tests/newgame.pad`, `tests/motion/`), teleports, `KF4_FX`
  (`game/screen_fx.c`; this step added `mapchange=N` and `savepoint`, which start the game's map change and open the
  save screen as a door and a save point do), and quick saves made up at each map's start (`tools/map_tour.py`'s).
- **The draws:** each scenario is played headless with **native draw off**, so that every pass is a GIF draw with its
  GS state: a GS capture of 4 vblanks at each moment (two game frames at 30, so that the next frame's reads show), and
  `kf4-gsreplay --list --events` of it. `--events` is new: the transfers (uploads, the frame's copies, read-backs) and
  the vblanks (which buffer is shown) between the draws, which `--list` did not have. `--list` now also gives a
  textured draw's texels (`UV`), its first vertex's colour (`RGBA`: a fill's colour, the brightness) and DTHE and
  COLCLAMP.
- **GPU time by draw** (the handoff's unsolved problem): `kf4-gsreplay --gpu-draws FILE` replays a capture with the PC
  renderer and numbers each draw for it as it is sent (`pcr_debug_gpu_draw`, a job on the renderer's thread: no
  flush, the batches made as without it). The canvas stamps each batch with its first and last draw, and the GPU's
  timer (`pcr_gpu_time.cpp`, on with `KF4_GPU_DRAWS`) writes every interval with them: each batch an interval of its
  own, the copies made for a batch (the frame's copy that a pass reading the frame needs) with its draws, and other
  work (uploads, local copies) with the draw after it. Each capture is replayed 12 times at the owner's scale, the
  first two not counted, the median kept. The numbering costs nothing on the GPU: 12 replays of 099 took 319.5 ms
  with it and 325.0 without.
- **The live totals:** each scenario played in a **real window** with the owner's settings (a copy of the data folder,
  `KF4_READ_SETTINGS=1`, native draw on), at 30 and at 60, with a 4 s profile by shader state (`KF4_GPU_DETAIL=1`). A
  game frame's cost is twice the 30 run's ms a picture; a picture between's is twice the 60 run's less the 30 run's.
- **The classes**, each channel (RGB, alpha, Z) a layer writes followed forward through the draws, transfers and
  scanouts after it, the next frame's included (the rules: tools/README.md):
  - (a) a whole-buffer clear: a flat colour or Z over the whole buffer, nothing tested;
  - (b) dead: everything it writes is written over before anything reads it;
  - (c) foldable into the scanout: a per-pixel colour op (untextured, or reading its own buffer at its own pixel), its
    alpha and Z unread, its colour read only by the scanout and by later such layers, nothing else drawn over it;
  - (d) must stay in memory: read as a texture, copied, or read by the next frame;
  - (e) other, with the reason.

## The layers

Costs are GPU ms a game frame at the owner's scale (2x, native widescreen), from the replays (native draw off): the
median over the captures where the layer showed, its draws and the copies made for them. A picture between draws every
layer again but the dead ones (the pump's `pcr_dead_writes.h`), so its cost is the same column for (a), (c), (d) and
(e). "Every frame" means every game frame of play in every map of the tour.

### 1. The swap's two clears (every frame)

- **What:** `Display_SwapBuffers` (`decomp/src/game/core/display.c`) calls libgraph's `sceGsSwapDBuff` twice: for
  `game_dbuff` and for `game_dbuff_context2` (`core/system.c`, `Display_Init`: a second double buffer that draws
  through GS context 2). Each sends its draw environment with a clear sprite (`sceGsSetDefClear`,
  `port/sdk/libgraph.c`). PATH3.
- **GS state:** a 640x448 sprite into the frame drawn (FBP 0 or 140), no blend, no alpha test, ZTST ALWAYS with Z
  written (ZBP 280); colour the map's clear colour (0x14, 0x14, 0x0F at 099), alpha 0x80, Z 0. The second is the same
  sprite in context 2: the same buffer, values and rectangle, nothing between.
- **Cost:** 0.46 (the first), 0.47 (the second); 0.002 for the second at 009, where the renderer recognises it as the
  same clear (`same_as_last_clear`, 95bd078c) and leaves it out. It does not at 099 and most maps (open questions).
  Each is already a `vkCmdClearAttachments` (`as_clear`), plus the depth rule's copy of Z cleared by a full-screen draw
  of its own (`clear_depth_copy`).
- **Readers:** the first, none: (b) dead in every judged frame. The second: the world's blends and depth tests, and the
  scanout where nothing is drawn: (a) a whole-buffer clear.
- **Exactness:** colour (8 bits a channel), alpha 0x80, Z 0, the stencil (DATE's) 0 and the depth rule's copy; the
  menus and the title clear their 640x224 field buffers the same way (0.22-0.29).

### 2. `GsDraw_ClearDepth`: Z 0, the colour kept (every frame)

- **What:** `gfx/gs_draw.c`: a 640x448 sprite whose pixels all fail the alpha test (TEST 0x32001: NEVER, FAIL
  ZB_ONLY), so it writes Z 0 alone. `GameLoop_DrawFrame` calls it after the screen passes and before draw list 3 and
  the arm; draw modes 1 and 2 (the map tint, the reflection) once more, after their first view.
- **Cost:** 0.19 (the renderer turns it into a depth clear already: 95bd078c).
- **Readers:** the arm, a held item (`MapItem_DrawPicked`) and draw list 3 test Z after it. In the tour nothing did in
  all but a few judged frames (the player had no weapon out, or the arm was not drawn): (b) dead. After the reflection
  (009) and with the herb in hand: (a) a depth clear.

### 3. `GsDraw_SetScreenAlpha(0)`: the destination alpha set, the colour kept (every frame)

- **What:** `gfx/gs_draw.c`, from `Entity_DrawPoolModels` (`entity/entity_ai.c`) in the world draw: ten 64-pixel
  strips, ALPHA 0x68 with FIX 0 ((Cs - 0) * 0 + Cd: the colour as it was), TEST 0x31001 (NEVER, FAIL FB_ONLY: no Z),
  the vertices' alpha 0.
- **Cost:** 0.42.
- **Readers:** DATE: the entity pool's models (TEST_2 0x55001) and the HUD's DATE sprites, through the alpha's bit 7;
  the frame's copy to 0x2BC0 (it copies the alpha too).
- **Class:** (e) in 74 frames: it writes the alpha alone over the whole buffer, so it is not a fast clear while colour
  and alpha are one image. It would be free as a stencil clear if DATE's bit is a stencil (the PC renderer's DATE is a
  stencil already) and nothing else reads the alpha. (b) dead in 17 (the opening, the fade into play, the map viewers:
  no DATE draw after it).
- **Exactness:** the pool models write FBA 1, setting bit 7 where they draw, so each of their pixels is blended once.

### 4. `GsDraw_FillScreen`: fills, tints, fades (situational, but one is on at 099 every frame)

- **What:** `gfx/gs_draw.c`: an untextured sprite in ten strips, ALPHA 0x44 ((Cs - Cd) * As + Cd) or 0x48
  ((Cs - 0) * As + Cd), TEST 0x31001 (no Z). Callers in `GameLoop_DrawFrame`'s order: `Map_CoverScreenWithMapTint`
  (draw mode 1), `Player_DrawScreenFade`, `Player_DrawScreenTints` (the ailments, the red damage flash, the death
  fade), under water's green fill at 035, then after the HUD: MapDrawState's screen fill (script command 197),
  `Game_DrawFade` (with ZMSK 1), `Splash_Draw`, the message dim, the pause's shade. Seen in the tour: black at alpha 2
  every frame at 099 and the first maps (MapDrawState's fill: `KF4_FX=fill` replaces it), alpha 0x18-0x7E in fades,
  the dim and spell 76, red 0x10-0x50 (0x48) in the damage flash and spell 81, purple 0x30 and darkness 0x20 (tints),
  green (035 under water), white 0x40 (the screen fade).
- **Cost:** 0.54 (0x44 and 0x48 alike); `Game_DrawFade`'s 0.33.
- **Class:** (c) foldable where it is drawn after the HUD (the screen fill, the fades, the dim, the pause): read only
  by later fills, the brightness pass and the scanout. (e) where the HUD or the arm is drawn over it after (the tints,
  the damage flash, the screen fade, the green fill): foldable at its place, or into the scanout with the HUD kept
  out of it.
- **Exactness:** each layer's result is a byte: ((A - B) * C >> 7) + D a channel, clamped (COLCLAMP 1), and the next
  layer reads that byte; C above 0x80 is a factor above 1. The alpha written is As: after the HUD read by nothing, but a
  fill under the HUD (the death fade, the tints) sets the alpha the HUD's DATE sprites test, so a fold of those must
  still write that alpha bit (or its stencil).

### 5. `Game_ApplyBrightness`: the brightness pass (every frame, last)

- **What:** `core/game.c`: the frame drawn over itself in ten 64-pixel strips, TFX modulate, TCC 0, the vertices'
  colour the option (`game_options[10]`: 136 in a new game, 64 to 255), no blend, ZMSK 1, TEST 0x30000, nearest
  filtering, each texel at its own pixel. After the HUD, last in `GameLoop_DrawFrame`.
- **Cost:** 1.24, of it about 0.4-0.6 the renderer's copy of the frame for the pass to read (`texture copy`); 1.2 also
  in the menu loop's frames (its own pass of the menu's frame).
- **Readers:** the scanout; in a knock-back (`aim_speed`) or with bit 40, the next frame's `Game_BlendPreviousFrame`.
- **Class:** (c) in 91 judged frames, (d) in 2 (the previous frame blended in).
- **Exactness:** RGB = min(255, (C * 136) >> 7) a channel, alpha 0x80 (the vertex's, TCC 0); at 136 it is not the
  identity. The sprites end at y 447.5, which the GS draws as row 447 (it samples at whole pixels).

### 6. `Game_TintScreen`: the frame times a colour (situational)

- Under water (0x5E, 0x87, 0x92), after the arm and before the HUD; the King's Map (0xEA, 0xB2, 0x46). The brightness
  pass's state with another colour. **Cost** 1.32 (with its copy of the frame). **Class** (e): the HUD is drawn over
  it, so it folds at its place, or into the scanout with the HUD kept out.

### 7. `GsDraw_CopyFrame(0x2BC0)` and `Game_DrawWobblingScreen` (under water, the MP drain)

- **What:** the frame copied (BITBLT, 640x448, `gfx/gs_draw.c`) to 0x2BC0, pages 350-489: over the reflection's target
  and the pause's and map viewers' textures. Then `Game_DrawWobblingScreen` (`core/game.c`): 70 cells of a 7 x 10 grid
  from the copy, their corners moved by springs, ALPHA 0x44.
- **Cost:** the copy 0.42, the wobble 1.17.
- **Class:** the copy (d) (the wobble reads it); the wobble (e): it reads the copy at moved places, so a fold keeps the
  copy and samples it with the moved coordinates.
- **Seen:** forced (`KF4_FX=water`, `drain`, `drainramp`) at 099, 029, 034 and 035; real at 029's place 0 (the map
  viewer scenario stood in the canal's deep water).

### 8. `Game_BlendPreviousFrame`: the frame before (the knock-back)

- **What:** `core/game.c`: one sprite of the buffer shown (the frame before) at alpha 0x40, ALPHA 0x44, TEST 0x51001;
  while `aim_speed` > 0 (every hit's knock-back) and for MapDrawState bit 40.
- **Cost:** 0.67. **Class:** (e): it reads the other buffer. It makes the frame before's final picture (its brightness
  pass) a reader across frames: (d), the history image of a PC renderer.

### 9. The passes that read the frame at other pixels

- The one-row blend (`Game_BlendScreenOffsetDown`, `--flicker-filter`, bit 42; also the King's Map) and the view-turn
  shift (`--no-whole-frame`, bit 45): 1.18-1.24, (e) (reads its own buffer one row or a few pixels away; the PC turns
  both off by default).
- The haze layers (bit 44, never set by the game) 5.41 and the debug depth sprite (bit 41) 1.99: (e), unreachable.

### 10. `GsDraw_CopyFrameToTexture(0x3540)`: the Dark Succubus

- A 256x256 draw into 0x3540 (pages 426-457, inside the copy's range) from the frame as it is up to her model; her
  model reads it. **Cost** 0.79-0.82. **Class** (d). Forced (`KF4_FX=copytex`): she sleeps in a new game.

### 11. The reflection's target (009, 014) and `GsDraw_ClearFrame`

- `Map_SetOffscreenTarget` (FBP 350, 256x256 PSMCT16S at 0x2BC0) then `GsDraw_ClearFrame` (colour, Z kept): 0.056,
  (a). The reflected world draws with its Z in the frame's Z buffer at 256 wide (ZBP 280, pages 280-295), then
  `GsDraw_ClearDepth` clears the frame's Z (0.28, (a)) before the world.

### 12. The menus' loops (the menu, the save screen, trading)

- The shown frame grabbed into 0x3240 (1.79, (d)) when a menu opens; then each menu frame, 60 a second, into 640x224
  field buffers (FBP 0 and 70): the clear (0.22), the backdrop from the grab (1.0-1.4), a shade at alpha 0x40 (0.65),
  then the pages. The backdrop and shade are (e) (the pages are drawn over them; foldable at their place, or the
  backdrop kept as an image and shaded when read). `GameLoop_ShowGrabbedFrame` draws the grab into both buffers on
  the way out. No pictures between in these loops.


## Each scenario: ms a game frame by class

The capture's first judged game frame (its buffer shown within the capture), replayed at 2x with native widescreen.
"Layers" is (a) to (e) together; the rest of "Frame" is the world, the HUD and the other targets.

| Scenario | Forced | Frame | Layers | (a) | (b) | (c) | (d) | (e) | The (d) and (e) layers |
|---|---|---|---|---|---|---|---|---|---|
| title @760 |  | 13.94 | 3.27 | 0.44 | 1.07 | 1.76 | 0.00 | 0.00 |  |
| start @1800 |  | 13.10 | 3.38 | 0.48 | 0.67 | 1.81 | 0.00 | 0.41 | e GsDraw_SetScreenAlpha 0.41 |
| start native off @1800 |  | 12.33 | 3.24 | 0.48 | 0.63 | 1.74 | 0.00 | 0.39 | e GsDraw_SetScreenAlpha 0.39 |
| start spin @1800 |  | 11.97 | 3.42 | 0.48 | 0.67 | 1.83 | 0.00 | 0.44 | e GsDraw_SetScreenAlpha 0.44 |
| walk @1780 |  | 12.96 | 3.37 | 0.50 | 0.68 | 1.80 | 0.00 | 0.40 | e GsDraw_SetScreenAlpha 0.40 |
| pickup @1860 |  | 9.72 | 3.70 | 0.68 | 0.68 | 1.90 | 0.00 | 0.45 | e GsDraw_SetScreenAlpha 0.45 |
| menus @1805 |  | 8.09 | 3.92 | 0.00 | 0.00 | 0.00 | 3.92 | 0.00 | d the shown frame grabbed into 0x3240 1.79; d the menu loop's backdrop from the grab at 0x3240 1.03; d the menu loop's backdrop from the grab at 0x3240 1.10 |
| bright40 @1800 | yes | 12.82 | 3.33 | 0.50 | 0.65 | 1.77 | 0.00 | 0.41 | e GsDraw_SetScreenAlpha 0.41 |
| brightff @1800 | yes | 12.72 | 3.34 | 0.47 | 0.65 | 1.78 | 0.00 | 0.44 | e GsDraw_SetScreenAlpha 0.44 |
| flicker @1800 |  | 13.83 | 4.55 | 0.48 | 1.06 | 1.77 | 0.00 | 1.24 | e a pass reading the frame being drawn 1.24 |
| fx water @1800 | yes | 15.88 | 6.31 | 0.48 | 0.66 | 1.85 | 0.43 | 2.90 | e GsDraw_SetScreenAlpha 0.42; e Game_DrawWobblingScreen 1.17; e Game_TintScreen(0x5E, 0x87, 0x92) 1.32; d GsDraw_CopyFrame(0x2BC0) 0.43 |
| fx drain @1800 | yes | 14.31 | 4.93 | 0.46 | 0.70 | 1.77 | 0.43 | 1.57 | e GsDraw_SetScreenAlpha 0.41; e Game_DrawWobblingScreen 1.16; d GsDraw_CopyFrame(0x2BC0) 0.43 |
| fx prev @1800 | yes | 13.35 | 3.98 | 0.47 | 0.65 | 0.54 | 1.22 | 1.11 | e GsDraw_SetScreenAlpha 0.43; e Game_BlendPreviousFrame 0.68; d Game_ApplyBrightness 1.22 |
| fx aim @1800 | yes | 13.28 | 3.85 | 0.44 | 0.61 | 0.52 | 1.22 | 1.06 | e GsDraw_SetScreenAlpha 0.39; e Game_BlendPreviousFrame 0.67; d Game_ApplyBrightness 1.22 |
| fx haze @1800 | yes | 18.47 | 8.72 | 0.48 | 0.64 | 1.75 | 0.00 | 5.85 | e GsDraw_SetScreenAlpha 0.44; e Game_DrawHazeLayers or Game_DrawDepthTestSprite 5.41 |
| fx depth @1800 | yes | 14.49 | 5.20 | 0.44 | 0.62 | 1.74 | 0.00 | 2.40 | e GsDraw_SetScreenAlpha 0.40; e Game_DrawHazeLayers or Game_DrawDepthTestSprite 1.99 |
| fx updown @1800 | yes | 12.87 | 3.39 | 0.48 | 0.67 | 1.81 | 0.00 | 0.43 | e GsDraw_SetScreenAlpha 0.43 |
| fx shift @1800 | yes | 13.69 | 4.38 | 0.44 | 1.01 | 1.75 | 0.00 | 1.18 | e a pass reading the frame being drawn 1.18 |
| fx fill @1800 | yes | 12.74 | 3.24 | 0.44 | 0.61 | 1.76 | 0.00 | 0.43 | e GsDraw_SetScreenAlpha 0.43 |
| fx fade @1800 | yes | 12.93 | 3.54 | 0.44 | 1.03 | 1.75 | 0.00 | 0.33 | e GsDraw_FillScreen(r, g, b, a, 0x48): Game_DrawFade 0.33 |
| fx tint @1800 | yes | 14.17 | 4.37 | 0.44 | 1.10 | 1.75 | 0.00 | 1.08 | e GsDraw_FillScreen(r, g, b, a, 0x48) 0.55; e GsDraw_FillScreen(r, g, b, a, 0x44) 0.53 |
| fx dim @1800 | yes | 12.79 | 3.51 | 0.47 | 0.63 | 2.01 | 0.00 | 0.40 | e GsDraw_SetScreenAlpha 0.40 |
| fx copytex @1800 | yes | 14.52 | 4.19 | 0.48 | 0.68 | 1.83 | 0.77 | 0.43 | e GsDraw_SetScreenAlpha 0.43; d GsDraw_CopyFrameToTexture(0x3540) 0.77 |
| pond @2000 |  | 15.74 | 2.70 | 0.33 | 0.67 | 1.25 | 0.00 | 0.44 | e GsDraw_SetScreenAlpha 0.44 |
| forest @2000 |  | 8.91 | 3.01 | 0.50 | 0.66 | 1.30 | 0.00 | 0.54 | e GsDraw_SetScreenAlpha 0.54 |
| canal water @2000 | yes | 13.36 | 5.99 | 0.51 | 0.65 | 1.41 | 0.41 | 3.01 | e GsDraw_SetScreenAlpha 0.45; e Game_DrawWobblingScreen 1.17; e Game_TintScreen(0x5E, 0x87, 0x92) 1.39; d GsDraw_CopyFrame(0x2BC0) 0.41 |
| guardian water @2000 | yes | 17.87 | 5.49 | 0.50 | 0.69 | 1.22 | 0.41 | 2.67 | e GsDraw_SetScreenAlpha 0.40; e Game_DrawWobblingScreen 1.16; e GsDraw_FillScreen(r, g, b, a, 0x48) 0.56; e GsDraw_FillScreen(r, g, b, a, 0x44) 0.55; d GsDraw_CopyFrame(0x2BC0) 0.41 |
| palace @2100 |  | 16.56 | 2.82 | 0.51 | 0.69 | 1.22 | 0.00 | 0.41 | e GsDraw_SetScreenAlpha 0.41 |
| giant leader @2000 | yes | 9.09 | 4.62 | 0.52 | 0.70 | 1.33 | 0.40 | 1.66 | e GsDraw_SetScreenAlpha 0.47; e Game_DrawWobblingScreen 1.19; d GsDraw_CopyFrame(0x2BC0) 0.40 |
| succubus @2000 | yes | 9.94 | 4.38 | 0.50 | 0.65 | 1.32 | 0.82 | 1.08 | e GsDraw_SetScreenAlpha 0.53; d GsDraw_CopyFrameToTexture(0x3540) 0.82; e GsDraw_FillScreen(r, g, b, a, 0x44) 0.55 |
| flesh @2000 |  | 7.30 | 3.85 | 0.51 | 0.70 | 2.15 | 0.00 | 0.48 | e GsDraw_SetScreenAlpha 0.48 |
| icy @2000 |  | 6.84 | 3.04 | 0.49 | 0.70 | 1.37 | 0.00 | 0.48 | e GsDraw_SetScreenAlpha 0.48 |
| salamanders @2100 |  | 14.78 | 2.77 | 0.51 | 0.63 | 1.22 | 0.00 | 0.40 | e GsDraw_SetScreenAlpha 0.40 |
| fire elementals @2300 |  | 10.15 | 2.81 | 0.48 | 0.66 | 1.24 | 0.00 | 0.43 | e GsDraw_SetScreenAlpha 0.43 |
| fire genie @2300 |  | 5.80 | 3.02 | 0.47 | 0.68 | 1.41 | 0.00 | 0.46 | e GsDraw_SetScreenAlpha 0.46 |
| combat @2210 |  | 14.12 | 3.13 | 0.47 | 0.65 | 1.58 | 0.00 | 0.43 | e GsDraw_SetScreenAlpha 0.43 |
| first kill @2645 |  | 10.77 | 2.81 | 0.47 | 0.64 | 1.26 | 0.00 | 0.43 | e GsDraw_SetScreenAlpha 0.43 |
| death @3920 |  | 9.63 | 3.36 | 0.48 | 0.66 | 1.24 | 0.00 | 0.97 | e GsDraw_SetScreenAlpha 0.42; e GsDraw_FillScreen(r, g, b, a, 0x44) 0.55 |
| spell 3 @1925 |  | 13.27 | 3.33 | 0.48 | 0.65 | 1.78 | 0.00 | 0.42 | e GsDraw_SetScreenAlpha 0.42 |
| spell 16 @1925 |  | 13.32 | 3.39 | 0.47 | 0.66 | 1.83 | 0.00 | 0.43 | e GsDraw_SetScreenAlpha 0.43 |
| spell 36 @1905 |  | 13.24 | 3.34 | 0.47 | 0.66 | 1.80 | 0.00 | 0.41 | e GsDraw_SetScreenAlpha 0.41 |
| spell 61 @1925 |  | 12.81 | 3.43 | 0.49 | 0.67 | 1.82 | 0.00 | 0.44 | e GsDraw_SetScreenAlpha 0.44 |
| spell 76 @2030 |  | 13.21 | 3.94 | 0.48 | 1.10 | 1.80 | 0.00 | 0.56 | e GsDraw_FillScreen(r, g, b, a, 0x44) 0.56 |
| spell 81 @1925 |  | 18.24 | 3.79 | 0.46 | 1.03 | 1.75 | 0.00 | 0.55 | e GsDraw_FillScreen(r, g, b, a, 0x48) 0.55 |
| mapview 1 @1900 | yes | 4.86 | 2.58 | 0.44 | 0.44 | 1.69 | 0.00 | 0.00 |  |
| mapview 5 @2100 | yes | 12.84 | 5.75 | 0.50 | 0.68 | 1.26 | 0.42 | 2.89 | e GsDraw_SetScreenAlpha 0.44; e Game_DrawWobblingScreen 1.17; e Game_TintScreen(0x5E, 0x87, 0x92) 1.28; d GsDraw_CopyFrame(0x2BC0) 0.42 |
| map change @1830 | yes | 11.28 | 3.71 | 0.48 | 0.66 | 2.12 | 0.00 | 0.45 | e GsDraw_SetScreenAlpha 0.45 |
| save point @1760 | yes | 12.73 | 3.35 | 0.48 | 0.65 | 1.81 | 0.00 | 0.41 | e GsDraw_SetScreenAlpha 0.41 |
| teleport @1802 |  | 12.82 | 3.43 | 0.48 | 0.66 | 1.85 | 0.00 | 0.45 | e GsDraw_SetScreenAlpha 0.45 |

(Each scenario's capture with the most layer time; the report has every capture.)

**The 64 maps' starts** (a quick save made up at each map's start, a walk and a turn; two captures each): a game
frame 9.9 ms median (6.3-18.4), its layers 2.80 ms median (28%; at most 53%). Every map draws the two swap clears, the
brightness pass, `GsDraw_ClearDepth` (dead in all but one capture) and `GsDraw_SetScreenAlpha`. Beyond those: the
reflection's target and its clear at 008 and 009; under water at 029 (its start is in the canal's deep water: the
copy, the wobble and the tint, really reached); fills before the HUD at 025 and 035 (red, 0x48: the damage flash on
arrival), 038 and 041 (black 0x40 and 0x50), 101 (green and red); fills after the HUD at 039 (black 0x0D and 0x3F).

## 1x, 2x and 4x

Five scenarios replayed at 1x (`2x1`: 1280 x 448 a buffer, the owner's "1x" with native widescreen), 2x (`3x2`, the
owner's) and 4x (`6x4`: 3840 x 1792), the first judged game frame, ms:

| Scenario | Scale | Frame | Layers | (a) | (b) | (c) | (d) + (e) |
|---|---|---|---|---|---|---|---|
| start | 1x | 5.67 | 1.16 | 0.16 | 0.21 | 0.62 | 0.16 |
| start | 2x | 13.10 | 3.38 | 0.48 | 0.67 | 1.81 | 0.41 |
| start | 4x | 45.88 | 13.91 | 2.14 | 3.04 | 6.95 | 1.78 |
| pond | 1x | 8.06 | 1.01 | 0.13 | 0.22 | 0.51 | 0.15 |
| pond | 2x | 15.74 | 2.70 | 0.33 | 0.67 | 1.25 | 0.44 |
| pond | 4x | 48.48 | 10.64 | 1.11 | 2.99 | 4.77 | 1.77 |
| fx water | 1x | 6.39 | 2.15 | 0.15 | 0.20 | 0.61 | 1.18 |
| fx water | 2x | 15.88 | 6.31 | 0.48 | 0.66 | 1.85 | 3.33 |
| fx water | 4x | 55.59 | 24.51 | 1.80 | 2.90 | 6.97 | 12.83 |
| fx aim | 1x | 5.95 | 1.45 | 0.16 | 0.21 | 0.21 | 0.88 |
| fx aim | 2x | 13.28 | 3.85 | 0.44 | 0.61 | 0.52 | 2.28 |
| fx aim | 4x | 47.09 | 16.18 | 1.96 | 2.85 | 2.12 | 9.26 |
| palace | 1x | 10.02 | 0.96 | 0.19 | 0.20 | 0.43 | 0.14 |
| palace | 2x | 16.56 | 2.82 | 0.51 | 0.69 | 1.22 | 0.41 |
| palace | 4x | 43.29 | 11.08 | 1.91 | 2.71 | 4.78 | 1.68 |

The layers' ms go with the pixels (three times the pixels from 1x to 2x, four times from 2x to 4x), so their share of
the frame grows with the resolution: at 099, 20% at 1x, 26% at 2x, 30% at 4x; under water 34%, 40%, 44%. The world's
draws grow more slowly (fixed costs a draw). What the layers cost is memory traffic at the picture's size, the thing a
fold or a fast clear removes; at 4x the layers alone are 11-25 ms a game frame on this GPU.

## Full-screen costs that are not layers

The rules above take a layer to be a run of screen-pass draws (VU1's 0x276, PATH2 or PATH3). Three more costs are as
large as a layer and show in the live profiles:

- **Native draw's DATE.** `Entity_DrawPoolModels` draws the entity pool's models with DATE (TEST_2 0x55001) after
  `GsDraw_SetScreenAlpha`: 34 small draws at 099. In the real window with native draw on (the owner's setting),
  `native 000 | untextured | blend (Cs-Cd)*FIX+Cd | DATE stencil | Z >= write` cost 0.33 ms a picture at 30, so 0.66
  a game frame; with native draw off the same draws are not among the top rows (under 0.05). At 60 the state costs about
  0.7 ms a picture between with native draw on or off. A native batch takes the whole screen as what it draws over,
  so its DATE set-up (the stencil from the alpha) is probably full-screen: the same traffic as a layer, for a few
  hundred pixels.
- **The sky.** At 099 and the outdoor maps the sky is drawn by near full-screen strips textured from a scrolling
  texture's 128 x 128 target (page 468), which the renderer copies to read (`surface copy` in the shader state):
  `native 000 | surface copy modulate rgba | fog | ... | Z always write`, 0.42 ms a picture at 30 (0.84 a game frame).
- **The scanout and the present, every vblank** (`camera-whip-slowdown.md`): about 1.25 ms each a vblank at the
  owner's settings, 2.5 ms a game frame at 30 when the picture has not changed.

## Targets and the GS memory they share

Pages are 8 KB; a 640 x 448 32-bit buffer is 140 of them. From every capture's draws, Z tests, copies and texture reads
of drawn-into memory:

| Memory | Pages | What uses it | Shares pages with |
|---|---|---|---|
| Frame buffer 0 (FBP 0, 640 x 448, 32-bit) | 0-139 | every frame | the menu loops' first field buffer (FBP 0, 640 x 224); the frame's own reads (the brightness pass, the tints) |
| Frame buffer 1 (FBP 140) | 140-279 | every frame | the menu loops' second field buffer (FBP 70: pages 70-149, across both frame buffers) and their 32-bit Z (ZBP 140: pages 140-209) |
| The frame's Z (ZBP 280, 16-bit) | 280-349 | every frame | **the scrolling textures' Z** (128 wide, pages 280-283: every map) and **the reflection's Z** (256 wide, 280-295: 008, 009, 014), tested and written in the same pages each frame |
| The reflection's view (FBP 350, 256 x 256, 16-bit) | 350-365 | 008, 009, 014 | the frame's copy at 0x2BC0 |
| The frame's copy (0x2BC0, 640 x 448) | 350-489 | under water, the MP drain, the map change's fade, the pause and map viewers' textures | everything below 490: the reflection's view, the menus' grab, the Succubus' copy, scrolling targets at 370 (021, 027), 436 (000), 450, 466-481; the game uploads the effects' textures again after a copy (`Effect_UploadTextures`) |
| The menus' grab (0x3240) | 402-481 drawn | a menu, trading, the smith, the save screen | the frame's copy, the Succubus' copy, the scrolling targets |
| The Dark Succubus' copy (0x3540, 256 x 256) | 426-457 | 038 | the frame's copy, the menus' grab |
| The scrolling textures' targets (128 x 128 or 128 x 160) | 370-377, 436-443, 450-457, 466-481 | every map (the sky, water, lava, the flesh cavern) | each other (466 and 468 overlap), the copy's range |

So the boundaries an image-per-target design must keep: the two frame buffers each an image (the menu loops' field
buffers too, at other places in the same memory); the frame's Z an image the reflection and the scrolling targets'
draws test as well, each at their own width (the renderer already clears those aliases: `clear_depth_aliases`); the
0x2BC0 range as named images (copy, reflection, grab, Succubus, scrolling targets) with the frame's copy writing over
the others when it is made; and the history of the last picture for the frame before's blend.

## The real window: what the owner's settings cost

Each scenario with a live moment, in a real window with the owner's settings (native draw on), 4 s at 30 and at 60
(the game held 30 game frames a second in every run but the pause and the menus, which do not count frames). **A game
frame** is twice the 30 run's GPU ms a picture (its two vblanks); **a picture between** is twice the 60 run's less the
30 run's. Each cell is "a game frame / a picture between", by shader state:

- clears: `2d | untextured | Z always write` (the swap's clears and `GsDraw_ClearDepth`, already clears);
- the alpha's reset: `2d | untextured | blend (Cs-0)*FIX+Cd | Z always write`;
- fills: `2d | untextured | blend (Cs-Cd)*As+Cd | Z always write` (0 in the scenes started from the quick save, which
  has no screen fill);
- brightness and tints: `2d | surface copy modulate rgb | Z always`;
- DATE: `native 000 | untextured | blend (Cs-Cd)*FIX+Cd | DATE stencil | Z >= write`;
- the sky: `native 000 | surface copy modulate rgba | fog | blend (Cs-Cd)*As+Cd | Z always write`;
- texture copies: the frame or a target copied for a draw to read it (the brightness pass, the sky, the wobble);
- the pump's copies: `copy_surfaces`, the frame's colour and depth copied for its pictures between (and the frame's
  copy to 0x2BC0);
- scanout and present: the picture made and scaled to the window, twice a game frame.

| Scenario | A game frame | A picture between | Clears | Alpha's reset | Fills | Brightness, tints | DATE | Sky | Texture copies | The pump's copies | Scanout + present |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bright40 | 15.9 | 17.6 | 1.19 / 1.02 | 0.42 / 0.49 | 0.00 / 0.00 | 0.87 / 1.07 | 0.66 / 0.68 | 0.80 / 1.08 | 0.42 / 0.43 | 0.00 / 2.37 | 5.0 |
| brightff | 16.3 | 18.2 | 1.16 / 0.64 | 0.43 / 0.52 | 0.00 / 0.00 | 0.87 / 1.14 | 0.66 / 0.68 | 0.80 / 1.08 | 0.42 / 0.46 | 0.00 / 2.51 | 5.5 |
| canal water | 17.2 | 19.0 | 1.10 / 0.94 | 0.44 / 0.42 | 0.00 / 0.00 | 1.76 / 2.19 | 0.70 / 0.71 | 0.00 / 0.00 | 1.32 / 1.29 | 0.54 / 3.25 | 5.1 |
| combat | 17.5 | 23.7 | 1.64 / 1.00 | 0.44 / 0.54 | 0.00 / 0.00 | 0.87 / 1.26 | 0.76 / 0.86 | 0.81 / 1.16 | 0.91 / 1.00 | 0.00 / 5.89 | 5.4 |
| fire elementals | 15.8 | 18.3 | 1.16 / 0.82 | 0.46 / 0.44 | 0.00 / 0.00 | 1.01 / 1.03 | 0.75 / 0.87 | 0.00 / 0.00 | 0.77 / 0.82 | 0.00 / 2.48 | 5.7 |
| fire genie | 10.5 | 2.2 | 1.03 / 0.05 | 0.44 / 0.10 | 0.00 / 0.00 | 0.92 / 0.22 | 0.00 / 0.00 | 0.00 / 0.00 | 0.60 / 0.18 | 0.00 / 0.58 | 5.1 |
| first kill | 17.7 | 24.6 | 1.59 / 0.77 | 0.47 / 0.56 | 0.00 / 0.00 | 0.89 / 1.34 | 0.53 / 0.60 | 0.84 / 1.17 | 0.90 / 1.07 | 0.00 / 5.99 | 5.3 |
| flesh | 12.5 | 12.1 | 1.41 / 0.51 | 0.47 / 0.71 | 0.55 / 0.51 | 0.95 / 0.79 | 0.73 / 0.79 | 0.00 / 0.00 | 0.89 / 0.84 | 0.00 / 2.61 | 5.6 |
| flicker | 17.2 | 19.7 | 1.12 / 0.72 | 0.42 / 0.49 | 0.00 / 0.00 | 0.86 / 1.24 | 0.65 / 0.69 | 0.79 / 1.10 | 0.77 / 0.82 | 0.00 / 2.46 | 5.2 |
| forest | 14.2 | 14.7 | 1.69 / 0.55 | 0.48 / 0.49 | 0.00 / 0.00 | 0.87 / 1.11 | 0.00 / 0.00 | 0.00 / 0.00 | 0.84 / 0.92 | 0.00 / 2.80 | 5.5 |
| fx aim | 18.3 | 18.6 | 1.08 / 1.05 | 0.42 / 0.57 | 0.00 / 0.00 | 0.87 / 1.31 | 0.65 / 0.68 | 0.78 / 1.13 | 1.01 / 0.83 | 0.00 / 0.53 | 5.6 |
| fx copytex | 18.3 | 23.0 | 1.09 / 0.98 | 0.42 / 0.55 | 0.00 / 0.00 | 0.89 / 1.23 | 0.65 / 0.69 | 0.78 / 1.15 | 1.27 / 1.64 | 0.00 / 2.83 | 5.5 |
| fx depth | 19.0 | 22.3 | 1.16 / 0.67 | 0.43 / 0.48 | 0.00 / 0.00 | 0.88 / 1.33 | 0.66 / 0.68 | 0.79 / 1.12 | 1.14 / 1.10 | 0.00 / 2.55 | 5.5 |
| fx dim | 17.3 | 19.8 | 1.18 / 0.73 | 0.43 / 0.54 | 0.54 / 0.55 | 0.88 / 1.22 | 0.65 / 0.69 | 0.78 / 1.15 | 0.62 / 0.69 | 0.00 / 2.49 | 5.4 |
| fx drain | 18.1 | 17.4 | 1.13 / 0.81 | 0.42 / 0.48 | 0.00 / 0.00 | 0.95 / 1.23 | 0.53 / 0.59 | 0.78 / 1.09 | 0.97 / 0.84 | 0.53 / 0.51 | 5.3 |
| fx fade | 17.1 | 18.1 | 1.19 / 0.74 | 0.44 / 0.43 | 0.00 / 0.00 | 0.88 / 1.12 | 0.66 / 0.68 | 0.85 / 0.97 | 0.60 / 0.62 | 0.00 / 2.51 | 5.3 |
| fx fill | 16.6 | 20.3 | 1.01 / 0.98 | 0.39 / 0.57 | 0.51 / 0.57 | 0.89 / 1.18 | 0.65 / 0.70 | 0.75 / 1.14 | 0.62 / 0.61 | 0.00 / 2.54 | 5.3 |
| fx haze | 22.4 | 27.3 | 1.16 / 0.72 | 0.44 / 0.51 | 0.00 / 0.00 | 0.88 / 1.31 | 0.65 / 0.68 | 0.78 / 1.16 | 1.12 / 1.07 | 0.00 / 2.58 | 5.5 |
| fx prev | 18.7 | 17.7 | 1.20 / 0.96 | 0.45 / 0.48 | 0.00 / 0.00 | 0.95 / 1.28 | 0.65 / 0.67 | 0.81 / 1.10 | 1.04 / 0.59 | 0.00 / 0.54 | 5.6 |
| fx shift | 18.0 | 21.0 | 1.14 / 0.65 | 0.41 / 0.49 | 0.00 / 0.00 | 0.92 / 1.20 | 0.64 / 0.68 | 0.78 / 1.13 | 0.99 / 1.10 | 0.00 / 2.47 | 5.6 |
| fx tint | 17.9 | 20.8 | 1.14 / 1.04 | 0.42 / 0.53 | 0.52 / 0.56 | 0.88 / 1.26 | 0.65 / 0.70 | 0.78 / 1.22 | 0.62 / 0.68 | 0.00 / 2.43 | 5.5 |
| fx updown | 16.8 | 19.2 | 1.11 / 0.85 | 0.43 / 0.50 | 0.00 / 0.00 | 0.88 / 1.14 | 0.65 / 0.69 | 0.79 / 1.09 | 0.61 / 0.60 | 0.00 / 2.58 | 5.6 |
| fx water | 19.8 | 19.6 | 1.15 / 0.90 | 0.43 / 0.46 | 0.00 / 0.00 | 1.77 / 2.84 | 0.53 / 0.58 | 0.78 / 1.06 | 1.38 / 1.16 | 0.52 / 0.27 | 5.6 |
| giant leader | 15.0 | 18.7 | 1.15 / 1.06 | 0.48 / 0.59 | 0.00 / 0.00 | 0.89 / 1.18 | 0.70 / 0.87 | 0.00 / 0.00 | 0.88 / 0.98 | 0.53 / 3.29 | 5.3 |
| guardian water | 21.0 | 28.7 | 1.13 / 0.95 | 0.41 / 0.49 | 0.54 / 0.55 | 0.93 / 1.14 | 1.03 / 1.49 | 0.00 / 0.00 | 1.08 / 1.04 | 0.53 / 3.50 | 5.0 |
| icy | 11.8 | 9.8 | 1.34 / 0.50 | 0.49 / 0.45 | 0.00 / 0.00 | 0.86 / 1.08 | 0.00 / 0.00 | 0.00 / 0.00 | 0.84 / 0.96 | 0.00 / 2.63 | 5.9 |
| menus | 20.6 | -0.4 | 1.00 / 0.01 | 0.00 / 0.00 | 0.03 / -0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 3.60 / 0.03 | 0.00 / 0.00 | 5.1 |
| palace | 20.0 | 23.5 | 1.18 / 0.61 | 0.44 / 0.60 | 0.00 / 0.00 | 0.99 / 0.94 | 0.85 / 1.16 | 0.00 / 0.00 | 0.53 / 0.65 | 0.00 / 2.32 | 6.0 |
| pause | 5.4 | 0.0 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 0.00 / 0.00 | 5.4 |
| pickup | 14.4 | 17.6 | 1.57 / 1.25 | 0.46 / 0.56 | 0.55 / 0.56 | 0.86 / 1.22 | 0.68 / 0.72 | 0.03 / 0.08 | 0.63 / 0.96 | 0.00 / 2.89 | 4.9 |
| pond | 21.1 | 28.0 | 1.21 / 1.27 | 0.45 / 0.55 | 0.00 / 0.00 | 0.87 / 1.15 | 1.00 / 1.67 | 0.00 / 0.00 | 1.00 / 1.09 | 0.00 / 3.06 | 5.9 |
| salamanders | 20.0 | 26.1 | 1.15 / 0.66 | 0.41 / 0.45 | 0.00 / 0.00 | 0.93 / 1.02 | 0.67 / 0.78 | 0.00 / 0.00 | 0.63 / 0.71 | 0.00 / 2.48 | 5.9 |
| spell 16 | 18.8 | 22.9 | 1.56 / 0.75 | 0.43 / 0.55 | 0.55 / 0.55 | 0.87 / 1.32 | 0.67 / 0.73 | 0.84 / 1.20 | 0.99 / 1.06 | 0.00 / 3.04 | 5.6 |
| spell 3 | 18.5 | 22.5 | 1.46 / 0.91 | 0.44 / 0.53 | 0.56 / 0.55 | 0.87 / 1.39 | 0.67 / 0.72 | 0.83 / 1.28 | 0.95 / 1.14 | 0.00 / 2.96 | 5.6 |
| start | 16.2 | 16.1 | 1.11 / 0.81 | 0.42 / 0.47 | 0.00 / 0.00 | 0.86 / 0.89 | 0.66 / 0.70 | 0.84 / 0.85 | 0.43 / 0.46 | 0.00 / 2.38 | 5.4 |
| start native off | 15.8 | 17.9 | 1.41 / 1.35 | 0.43 / 0.52 | 0.00 / 0.00 | 0.87 / 0.94 | 0.00 / 0.00 | 0.00 / 0.00 | 0.41 / 0.50 | 0.00 / 2.47 | 5.4 |
| start spin | 15.0 | 27.4 | 1.24 / 1.83 | 0.44 / 0.94 | 0.00 / 0.00 | 0.87 / 2.08 | 0.67 / 1.39 | 0.81 / 1.85 | 0.43 / 0.91 | 0.00 / 2.41 | 5.2 |
| succubus | 14.9 | 17.1 | 1.17 / 0.92 | 0.45 / 0.61 | 0.56 / 0.57 | 0.89 / 1.18 | 0.00 / 0.00 | 0.00 / 0.00 | 1.17 / 1.18 | 0.00 / 2.82 | 5.2 |
| walk | 16.9 | 19.4 | 1.44 / 1.08 | 0.44 / 0.54 | 0.55 / 0.59 | 0.87 / 1.09 | 0.47 / 0.47 | 0.84 / 1.04 | 0.65 / 1.01 | 0.00 / 2.84 | 5.2 |

What the real window adds to the replays:

- **The layers cost what the replays say.** At 099 the clears 1.1 ms a game frame (the replay's 0.46 + 0.47 + 0.19), the
  alpha's reset 0.42, the brightness 0.87 and its copy, the fill 0.55.
- **A picture between costs as much as a game frame, or more** (16-28 ms against 15-21), and **the pump's copies are
  about 2.4-3 ms of each**: as much as all the layers together. Under water and in the MP drain the frame's copy to
  0x2BC0 adds 0.5 a game frame. These copies are not the game's: they are how the pump keeps the frame for its pictures.
- **Native DATE** is 0.5-1.0 ms a game frame and as much again a picture between in every scene in play (1.0-1.7 at the
  pond and the Guardian's Room), against nothing with native draw off.
- **Scanout and present** are about 5.4 ms a game frame at 30 (two vblanks), every scene.
- **A mouse spin** (`start spin`) makes a picture between 27.4 ms against 16.1 standing: every layer's row doubles in
  the 60 run (instant mouse look draws the vblank's picture again).
- **Fire genie** (025) at 60 drew few pictures between (2.2 ms "a picture between"): the adaptive cut dropped them.



## Open questions

1. **Why the repeated clear is still drawn at 099.** `same_as_last_clear` (95bd078c) leaves the second swap clear out at
   009 (0.002 ms) but not at 099 and most maps: "0 more left out, the same as the last" in the owner's real-window run
   at 099 and in every replay there. At 099 a second Z surface (the scrolling textures' 128-wide Z over page 280)
   exists and `clear_depth_aliases` clears it between the two clears; something on that path counts as a surface
   write. Not traced further (no renderer changes in this step).
2. **Native draw's DATE.** Live at 099 with native draw on, `native 000 | untextured | blend (Cs-Cd)*FIX+Cd | DATE
   stencil` cost 0.69 ms a picture; the same 34 small draws (the entity pool's models) cost 0.07 ms a game frame
   replayed as GIF batches. A native batch takes its scissor (the whole screen) as what it draws over, so its DATE
   set-up may be full-screen. The native-off live run (`start native off`) against `start` confirms the game frame's
   part: 0.66 ms a game frame with native draw on, not among the top rows with it off. In pictures between the DATE
   batch costs about 0.7 ms either way, so the pump's handling of it is costly too. Where the time goes inside it (the
   stencil's set-up over the whole scissor?) is not traced.
3. **The sky.** At 099 the sky is about 1 ms a game frame of near full-screen world strips read from the scrolling
   texture at page 468 (live: `native 000 | surface copy ... | Z always write`, 0.89 ms a picture): not a layer by the
   rules here (a world draw), but it is a texture read of a render target, so it pays a copy of that target each frame.
4. **Fast clears of these values.** The frame's clear colour is the map's (0x14, 0x14, 0x0F, alpha 0x80 at 099), not 0
   or 1. Whether each GPU's fast clear takes any colour, and a 16-bit or 32-bit depth's 0, is to be checked per GPU
   family; the inventory only says which layers are whole-buffer clears.
5. **The 9070 XT** was not run. The ms here are the Vega 7's on the balanced profile.
6. **Not reached in play:** the deadly water's death screen (a death by a creature was reached instead), a door's map change (forced
   with `KF4_FX=mapchange`, the same `MapChange_RunToEnd` path), a real save point (forced with `KF4_FX=savepoint`),
   the Dark Succubus awake (forced with `copytex`), the Green Wand's passage (the same capture and fade), spells 36, 61
   and 76 showed no layer of their own beyond 76's fills. The haze and the depth sprite are unreachable in the game.
7. **The menu loops' passes** were classed by the general rules; their backdrop is a grab drawn every menu frame at
   60, and a PC renderer would keep the grab as an image and shade it when read. Their cost is small next to play (no
   pictures between, 640x224 buffers).


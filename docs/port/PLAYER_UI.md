# The player menu (RmlUi)

The menu players use: Holyland's settings in King's Field IV's look, drawn with
[RmlUi](https://github.com/mikke89/RmlUi) over the game. **F1** (or a controller's **L3 + R3**) opens and closes it. The
Dear ImGui overlay is now the developers' (**Shift+F1**), and the long-term goal is to retire ImGui entirely (the
plan is below).

Started 2026-11-18 on `feat/player-ui`. Look and layout drew on Soulbrandt's (the Demon's Souls PC port) for what a
port's menu needs to do: settings as data, a help panel, and moving by keyboard, controller and mouse alike. Its look
is not borrowed.

## What it does now

- Laid out (since 2026-11-20) as a sidebar, the rows and the help: the Holyland logo at the top left over the
  sections (Display, Graphics, Controls, Key Bindings, Gameplay, Map, Hands, Saves, Sound & Menu, Cheats) and the
  version; the section's title and rows in the middle; the focused row's help on the right. Along the bottom, the keys
  for what you can do now: the keyboard's, or the controller's once it was used.
- Every player's setting the overlay had is here: the window size, the PS2's picture fixes, instant mouse look and
  the mouse in menus, the key bindings, the extended stats, the automap and its map help, the hands, the update notice
  (shown only when one is due) and the cheats. Left in the overlay only: the overlay's size, the presenter, save states,
  native draw kinds, the debugging tools and teleport.
- **Key bindings**: a row for each action in use, with its three slots (two keys or mouse buttons, a controller
  button). Left / Right choose the slot, Enter (or a click on a slot) waits for the next key or button
  (`port_controls_listen`; `port/sdk/controls.c` takes it before the menu sees it), Delete (a controller's Square,
  a right click on a slot) empties it, F1 or five seconds stop the waiting.
- **Keyboard**: Up / Down (or W / S) move; Left / Right (A / D) change the value in place; Enter (or Space) opens a
  choice's list, turns a toggle over, or does an action; Esc (Backspace) closes the list, then the menu; Q / E (Page
  Up / Down, Tab / Shift+Tab) change the section; Delete empties a binding's slot; F1 closes. F11 still switches full screen.
- **Controller**: d-pad or left stick (held: repeats), Cross (A) chooses, Circle (B) backs out, Square (X) empties a
  binding's slot, L1 / R1 change the section, Start or L3 + R3 close it.
- **Controller pictures** (2026-11-21): once a controller was used, the prompts along the bottom, the section keys
  in the sidebar and the controller slot of each key binding show its buttons as pictures, in its own family's look:
  PlayStation's (Cross, Circle, L1), Xbox's (A, B, LB; also Steam Deck and other controllers) or a Switch's (B, A, L,
  where SDL's south, east and left shoulder sit). The family comes from SDL's type of the controller that was pressed
  (`SDL_GetGamepadTypeForID`), and of the first one open when the menu opens. Pictures from Kenney's Input Prompts
  fonts (CC0), outlined; `port/ui/pad_glyphs.hpp` holds each family's table. Keyboard keys keep their boxes.
- **Mouse**: over a row focuses it; a click is Enter; the arrows either side of a value step it; a click on a list's
  choice picks it; a click on a tab shows it; the right button is Back; the wheel scrolls.
- At the start, a small panel at the top of the screen says "F1 or L3 + R3: Holyland's settings" for ten seconds
  (fading over the last one), and goes as soon as the menu is opened. Not under a pad script (`--input`), so test
  runs' pictures are as they were. It is the menu's document with `body.closed`: only `#hint` shows.
- **Sounds**: the game's own menu sounds, the first four of its se_com bank, as King's Field IV's menus play them
  (`Sound_PlayMenu`) and at its sound volume option: the cursor's for a move, a list's highlight, a tab or a value
  that changed; Cross's for opening the menu or a list, a choice, a toggle, an action, a slot listened for (and the
  key that filled it); Circle's for closing the menu or a list and emptying a slot; the refusal for Confirm on a
  row with nothing to do and a click on a dimmed row. Nothing when nothing changed (Up at the top). The model gives
  each input its cue (`MenuCue`, tested in `ui_menu_test`); `game/menu_sounds.c` plays it and sends it to the
  sound driver at once, so it is heard while the menu holds the game paused. Nothing plays before the game has
  loaded its sounds or while it loads them again (`Sound_Loaded`, patched into `sound/sound_game.c`). The test
  pictures' presses make no sound.
- A row whose value came from the command line says so ("command line"), and its help explains it.
- While it is open the game sees no input, and is paused if "Pause while open" is on (the same setting the overlay
  used).
- It is the top layer: drawn over the ImGui layers (the full map, the extended stats beside the game's menus), and it
  has the input over them. Opened over the full map, the map waits under it and has the keys (M, Esc) back when it
  closes. Who has the input, and so whether the game is blocked and the mouse let go, is decided in one place,
  `port_overlay_holds_input` (port/overlay/overlay.cpp), which port/sdk/controls.c asks at each vblank; the layers
  only open and close.
- Changes apply as the overlay's did (`port_live_set`, `port_settings`, `port_options`) and settings.json is written
  as before. The menu itself saves nothing of its own.
- **Game speed** (Gameplay) is live: its row goes through `port_live_set(PORT_LIVE_GAME_SPEED, ...)` and
  `port/platform/kernel.c` re-anchors the vblank pacing when the clock a vblank takes changes, so 1x to 2x in
  quarters is chosen and heard in play, either way; `KF4_SET_AT=VBLANK:game_speed=N` drives the same path in
  test runs. The clock runs the speed only while the play simulates: `game/frame_interp.c`, at its hook in the
  patched `GameLoop_DrawFrame`, marks the clock (`port_clock_played` / `port_clock_stopped`,
  `port/platform/video.c`) from `Quick_WhyNotInPlay` (`game/quicksave.c`), so the game's pause, an open
  message, a menu, a map or screen fade and a movie's loop keep the PS2's 1x whatever the row says.

## Files

| file | what |
| --- | --- |
| `port/include/port/player_ui.h` | the C entry points: init, events, open / visible, draw, test pictures |
| `port/ui/player_ui.cpp` | RmlUi set up; the page written as RML from the rows and the model; input |
| `port/ui/menu_model.hpp/.cpp` | moving about: focus, lists, tabs, binding slots, the pointer, each input's sound cue (no RmlUi, no settings): `tests/ui_menu_test.cpp` |
| `port/include/port/menu_sounds.h`, `game/menu_sounds.c` | the game's menu sounds, played for the menu |
| `port/ui/pad_glyphs.hpp/.cpp` | a controller's buttons as pictures: the family of an SDL controller type, each family's code points in Kenney's fonts, a binding's button: `tests/ui_pad_glyphs_test.cpp` (checks the face buttons against SDL's own labels) |
| `port/ui/settings_rows.hpp/.cpp` | **the settings, as data**: tabs and rows |
| `port/ui/ui_render.hpp/.cpp` | `ui::Recorder`, RmlUi's `RenderInterface`: geometry and textures recorded into a `ui::Frame` |
| `port/ui/ui_frame.hpp` | `ui::Frame`: one picture's triangles, draws, texture uploads and releases (no Vulkan) |
| `port/platform/present_vk_ui.hpp/.cpp` | the Vulkan presenter's drawing of a `ui::Frame`: pipeline, textures, buffers |
| `port/ui/shaders/ui.vert`, `ui.frag`, `ui_spirv.h` | its shaders; `tools/ui_shaders.sh` writes `ui_spirv.h` (committed) |
| `port/ui/res/menu.rml`, `kf4.rcss` | the document and its look |
| `port/ui/art/*.png`, `tools/ui_ornaments.py`, `tools/ui_masks.py`, `port/ui/res/*.alpha` | the logo and the ornaments (the window's corners, the soft box the sidebar's shade fades with): `ui_ornaments.py` draws the ornaments, `ui_masks.py` makes the alpha masks the game carries from its table (all committed; run them after changing a picture) |
| `port/ui/game_menu_art.hpp/.cpp` | the game's own menu art: `menu/menu.tmx`'s textures read from the disc and decoded for RmlUi (`game-menu/<tbp>-<look>.tex`): `tests/ui_game_art_test.cpp` |
| `port/ui/res/fonts/` | Cinzel (titles; static Regular and SemiBold cut from the variable font with fontTools' `varLib.instancer`) and Spectral (text), SIL OFL 1.1; `kenney_input_{xbox_series,playstation_series,nintendo_switch}.ttf` (controller pictures, Kenney's Input Prompts 1.5, CC0: `CC0-Kenney-Input-Prompts.txt`; their glyphs are in the private use area from U+E000, so each is its own family) |
| `cmake/rmlui.cmake`, `cmake/ui_assets.cmake` | RmlUi and FreeType built from `third_party/`; `res/` carried in the game as arrays |
| `third_party/rmlui`, `third_party/freetype` | copied in (see their `UPSTREAM.md`), so a release builds offline |

It is built with the Vulkan presenter only (`KF4_HAVE_PLAYER_UI`). With `--presenter sdl` or `--renderer gs` there
is no player menu and F1 opens the overlay as before.

## Adding a setting

Add a `SettingRow` to its tab's array in `port/ui/settings_rows.cpp`: label, help, kind (`choice`, `toggle`,
`number`, `action`, `heading`, `binding`), its choices or range, and `get` / `set` (or `act`). Optional: `describe`
(text for the value: "Auto (2x)"), `shown` (hide the row), `enabled` (dim it), `overridden` (the command line chose
this run's value), `note` (a changing line under the help: the cheats' last message). A `binding` row names its
`action` (a `PortAction`) and is shown while `port_controls_action_used`; the Key Bindings rows are made from
`ACTION_COUNT`, with a heading before each group in `binding_groups`. A new section is a new entry in `tabs`. Nothing
else changes: the model and the page take any row.

## The look

`kf4.rcss` has the palette at its top: cold iron and ice, taken from the game's own window frame (2026-11-21; before
it dark iron and amber, and before that blue-grey and IM FELL English). The game's black panel and wisps of ice for the
window (the iron frame and corners only when the disc lacks them), titles in Cinzel in ice blue, a soft glow fading in
from the left for the focused line (no bars or diamond rules, 2026-11-21), text in cool white; one accent throughout, ember only for warnings and
a key being listened for. RmlUi has no default style sheet:
every element is inline until the sheet says otherwise (hence `div, h2, p { display: block }`).

The recorder draws triangles, textures and scissor clipping only. So the sheet uses borders, colours and the
straight gradients (`horizontal-gradient`, `vertical-gradient`, made of vertex colours). It does not use
`linear-gradient` and the other shader gradients, `box-shadow`, `filter`, `transform` or `mask-image`: RmlUi then
leaves them out, and the recorder logs the first of each kind. The only images are the menu's own alpha masks
(`res/*.alpha`: "KF4A", width, height, a byte of alpha a pixel), white and coloured by `image-color`, so the game needs
no image decoder; the logo, the corners and the soft box are. Textures RmlUi makes between pictures (loading the document) go with the next
picture.

## Testing

- `ctest -R ui_menu`: the model.
- Pictures without anyone at the screen: `KF4_OVERLAY_SHOT=ui-<tab>[:keys]@vblank,...` opens the menu on a tab,
  presses the keys once (`up down left right confirm back next previous clear`, separated by `.`; `xbox`,
  `playstation`, `nintendo` or `keyboard` show the prompts as if that was used last; the sections are `display graphics controls keys gameplay interface general cheats`), and writes
  `overlay_ui-<tab>[:keys].png` into `--dump-dir`. Use a scratch `KF4_DATA_DIR`, since the keys change settings and
  the run writes them:

  ```sh
  KF4_DATA_DIR=/tmp/ui-data KF4_OVERLAY_SHOT="ui-display:down.down.down.confirm@1300,ui-graphics@1330,closed@1360" \
    out/linux-clang/kf4 --disc <image> --input tests/newgame.pad --teleport 900:009 --invincible --no-audio \
    --saves none --frames 1400 --dump-dir /tmp/ui-shots
  ```
- Checked on 2026-11-18: ASan + UBSan run (`out/linux-clang-asan`), with no reports from the menu, RmlUi or
  FreeType; Vulkan validation layer, with no errors from the menu's drawing.

## Retiring ImGui: the plan

Still in ImGui (`port/overlay/`), to move over one at a time:

1. ~~**Key bindings**~~, 2. ~~**the mods' own settings**~~, 3. ~~**window size and the PS2 picture fixes**~~: moved
   on 2026-11-20 (the Key Bindings, Gameplay, Map, Hands, Display and Graphics sections). The overlay still has them
   too, for developers, until it goes.
4. **Notices**: the save-state, quick-save and debug toasts, the "building pipelines" notice, the update note and
   badge (`update_badge.cpp`). These are a toast layer in RmlUi, visible while the menu is closed
   (`port_player_ui_visible`).
5. **In-play views**: the automap's full map and minimap, the stats panel over the game's menu. These need images
   (`LoadTexture` / textures the game makes) in the recorder.
6. **Developer tools**: the Developer page, the profiler (Shift+P) and the frame views. These can stay in ImGui
   longest, or become an RmlUi developer tab. Once they are moved, `third_party/imgui` and the ImGui backends
   go.
7. **The SDL presenter** (`--presenter sdl`): draw `ui::Frame` with `SDL_RenderGeometryRaw` (the launcher does
   this already: `launcher/window.cpp`, `Window::State::draw`) (premultiplied blend
   mode), so the menu exists there too.

The game's own art from the disc (2026-11-21, a trial on `feat/ui-game-art`): when the disc has `menu/menu.tmx`, the
window is dressed in King's Field IV's own menu window (`body.game-art` in `kf4.rcss`): its soft black panel and the four
wisps of ice added along its sides, the pieces and places of the game's Settings screen (`menu/option_para.dat`, parts 0
and 31 to 34). Each part of a `menu/*_para.dat` is a 0x60-byte `TexturedRectDef` (two corners in 12.4 with 0x6C00 and
0x7900 off, ST, colour, TEX0) whose TBP0 and CBP name one of `menu.tmx`'s textures; `Menu_SetBlendMode` 0 blends
over, 1 adds. Not yet: the game's font, the title and line glows (their words are drawn into the textures).

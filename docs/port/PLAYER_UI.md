# The player menu (RmlUi)

The menu players use: Darker Reality's settings in King's Field IV's look, drawn with
[RmlUi](https://github.com/mikke89/RmlUi) over the game. **F1** (or a controller's **L3 + R3**) opens and closes it. The
Dear ImGui overlay is now the developers' (**Shift+F1**), and the long-term goal is to retire ImGui entirely (the
plan is below).

Started 2026-11-18 on `feat/player-ui`. Look and layout drew on Soulbrandt's (the Demon's Souls PC port) for what a
port's menu needs to do: settings as data, a help panel, and moving by keyboard, controller and mouse alike. Its look
is not borrowed.

## What it does now

- Tabs: Display, Graphics, Controls, Gameplay, Saves, Sound & Menu. The rows on the left, and the focused row's
  help on the right. Under the rows, the keys for what you can do now: the keyboard's, or the controller's once it was
  used.
- **Keyboard**: Up / Down (or W / S) move; Left / Right (A / D) change the value in place; Enter (or Space) opens a
  choice's list, turns a toggle over, or does an action; Esc (Backspace) closes the list, then the menu; Q / E (Page
  Up / Down, Tab / Shift+Tab) change the tab; F1 closes. F11 still switches full screen.
- **Controller**: d-pad or left stick (held: repeats), Cross (A) chooses, Circle (B) backs out, L1 / R1 change the
  tab, Start or L3 + R3 close it.
- **Mouse**: over a row focuses it; a click is Enter; the arrows either side of a value step it; a click on a list's
  choice picks it; a click on a tab shows it; the right button is Back; the wheel scrolls.
- A row whose value came from the command line says so ("command line"), and its help explains it.
- While it is open the game sees no input, and is paused if "Pause while open" is on (the same setting the overlay
  used).
- Changes apply as the overlay's did (`port_live_set`, `port_settings`, `port_options`) and settings.json is written
  as before. The menu itself saves nothing of its own.

## Files

| file | what |
| --- | --- |
| `port/include/port/player_ui.h` | the C entry points: init, events, open / visible, draw, test pictures |
| `port/ui/player_ui.cpp` | RmlUi set up; the page written as RML from the rows and the model; input |
| `port/ui/menu_model.hpp/.cpp` | moving about: focus, lists, tabs, the pointer (no RmlUi, no settings): `tests/ui_menu_test.cpp` |
| `port/ui/settings_rows.hpp/.cpp` | **the settings, as data**: tabs and rows |
| `port/ui/ui_render.hpp/.cpp` | `ui::Recorder`, RmlUi's `RenderInterface`: geometry and textures recorded into a `ui::Frame` |
| `port/ui/ui_frame.hpp` | `ui::Frame`: one picture's triangles, draws, texture uploads and releases (no Vulkan) |
| `port/platform/present_vk_ui.hpp/.cpp` | the Vulkan presenter's drawing of a `ui::Frame`: pipeline, textures, buffers |
| `port/ui/shaders/ui.vert`, `ui.frag`, `ui_spirv.h` | its shaders; `tools/ui_shaders.sh` writes `ui_spirv.h` (committed) |
| `port/ui/res/menu.rml`, `kf4.rcss` | the document and its look |
| `port/ui/res/fonts/` | IM FELL English (titles) and Spectral (text), SIL OFL 1.1 |
| `cmake/rmlui.cmake`, `cmake/ui_assets.cmake` | RmlUi and FreeType built from `third_party/`; `res/` carried in the game as arrays |
| `third_party/rmlui`, `third_party/freetype` | copied in (see their `UPSTREAM.md`), so a release builds offline |

It is built with the Vulkan presenter only (`KF4_HAVE_PLAYER_UI`). With `--presenter sdl` or `--renderer gs` there
is no player menu and F1 opens the overlay as before.

## Adding a setting

Add a `SettingRow` to its tab's array in `port/ui/settings_rows.cpp`: label, help, kind (`choice`, `toggle`,
`number`, `action`, `heading`), its choices or range, and `get` / `set` (or `act`). Optional: `describe` (text for
the value: "Auto (2x)"), `shown` (hide the row), `enabled` (dim it), `overridden` (the command line chose this run's
value). Nothing else changes: the model and the page take any row.

## The look

`kf4.rcss` has the palette at its top. Windows of near-black blue-grey with a double steel rule, titles in pale gold
in IM FELL English SC, a cold blue band for the focused line, text in bone white. RmlUi has no default style sheet:
every element is inline until the sheet says otherwise (hence `div, h2, p { display: block }`).

The recorder draws triangles, textures and scissor clipping only. So the sheet uses borders, colours and the
straight gradients (`horizontal-gradient`, `vertical-gradient`, made of vertex colours). It does not use
`linear-gradient` and the other shader gradients, `box-shadow`, `filter`, `transform`, `mask-image` or images from
files: RmlUi then leaves them out, and the recorder logs the first of each kind.

## Testing

- `ctest -R ui_menu`: the model.
- Pictures without anyone at the screen: `KF4_OVERLAY_SHOT=ui-<tab>[:keys]@vblank,...` opens the menu on a tab,
  presses the keys once (`up down left right confirm back next previous`, separated by `.`), and writes
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

1. **Key bindings** (Controls: the table with listening for a key). It needs a "press a key" row kind and a
   bindings page.
2. **The mods' own settings**: the extended stats (`stats_panel.cpp`), the automap (`automap_view.cpp`) and the hands
   (`hands_view.cpp`) sections. They become rows on Gameplay.
3. **Window size** (the list of sizes the screen fits) and the PS2 picture fixes (Developer > Renderer).
4. **Notices**: the save-state, quick-save and debug toasts, the "building pipelines" notice, the update note and
   badge (`update_badge.cpp`). These are a toast layer in RmlUi, visible while the menu is closed
   (`port_player_ui_visible`).
5. **In-play views**: the automap's full map and minimap, the stats panel over the game's menu. These need images
   (`LoadTexture` / textures the game makes) in the recorder.
6. **Developer tools**: the Developer page, the profiler (Shift+P) and the frame views. These can stay in ImGui
   longest, or become an RmlUi developer tab. Once they are moved, `third_party/imgui` and the ImGui backends
   go.
7. **The SDL presenter** (`--presenter sdl`): draw `ui::Frame` with `SDL_RenderGeometryRaw` (premultiplied blend
   mode), so the menu exists there too.

Not started yet: the game's own art from the disc (the menu frames and fonts KF4 draws with), read at run time like
the rest of the game's data, to make the menu look even more like the game's own.

# Modding Holyland

A mod is a folder in Holyland's mods folder (`<data folder>/mods/<mod>`: `%APPDATA%\holyland\mods\` on Windows,
`~/.local/share/holyland/mods/` on Linux). It can change the game's files and text with no code at all, add settings and
keys, and run its own code (a plugin) that is told what happens in the game, reaches the game's own functions and
memory, replaces game functions, and draws its own pages in the player menu. The player turns mods on and off on the
player menu's **Mods** page; a mod with code starts off.

| Layer | What the mod has | Where |
| --- | --- | --- |
| Data | `files/` (whole KF4.DAT members), `text.json`, `signs/` | [MODS.md](MODS.md) |
| Manifest | `mod.json`: name, version, settings, keys, plugin | below |
| Plugin | a `.dll` (Windows) or `.so` (Linux) with `hl_mod_init` | [modding/include/holyland_mod.h](../../modding/include/holyland_mod.h) |

## mod.json

```json
{
  "name": "Journal", "version": "1.0", "author": "...", "description": "...",
  "holyland": "0.2.1",
  "api": 1,
  "plugin": { "windows-x64": "bin/windows-x64/journal.dll", "linux-x64": "bin/linux-x64/journal.so" },
  "settings": [
    { "key": "show_all", "label": "Show Everything", "help": "...", "type": "toggle", "default": 0 },
    { "key": "size", "label": "Size", "type": "choice", "choices": ["Small", "Large"], "default": 0 },
    { "key": "speed", "label": "Speed", "type": "number", "min": 1, "max": 8, "step": 1, "default": 2 }
  ],
  "keys": [ { "key": "open", "label": "Journal", "default": "B" } ]
}
```

- Everything is optional: a folder with no `mod.json` is a data mod named by its folder, on by default.
- **settings** are shown under the mod on the Mods page (once it is on) and kept in `settings.json`'s
  `"mod_list"`. Toggles are 0 or 1, choices their index, numbers whole numbers in their range.
- **keys** are listed on Key Bindings under "Mods' keys" and rebound there like Holyland's own. `"default"` is a
  key's name (`"B"`, `"F7"`) or a binding as `settings.json` writes them (`"key:B"`, `"pad:..."`). 16 keys for all
  mods together.
- **plugin** names the plugin for each system; a mod whose plugin is not built for this one still gives its data.
  **api** is the API major it was built for (Holyland refuses another); **holyland** the release it was built
  against (Holyland warns when it differs: see "Stability").

On and off, and a mod's plugin, take effect at the next start.

## Plugins

A plugin exports `hl_mod_init(const HlApi *api, HlMod *mod)` and returns `HL_API_MAJOR`. The API
(`modding/include/holyland_mod.h`, which documents each call) gives it:

- **About itself**: its folder (to read its own files), a data folder of its own (`<data folder>/mod-data/<mod>`), its
  settings and keys.
- **Events**: the game's frames (`HL_EVENT_FRAME`, and `HL_EVENT_DRAW` last in the frame, over the HUD), the title, a
  new game, saves written and loaded (with a key to name the mod's file for that save), and the menus' events for a
  page of its own.
- **The game**: game (EE) addresses to pointers and back, memory in game RAM (`game_alloc`: what the game's code draws
  from or keeps must be there), any of the game's or Holyland's functions and variables by name (`symbol`), and
  **hooks**: a game function replaced by the mod's, which calls the original when it wants (before, after or not at
  all). Several mods can hook one function: they chain in load order.
- **Pages**: the player menu's RmlUi context (`ui_context`) to load the mod's own documents in, and `ui_set_open`:
  while a mod's page is open the menu stays closed and the mod gets the input.

Threads: the game's events run on the game's thread, where the game's functions may be called; the `HL_EVENT_UI_*`
events on the menus' thread.

### How hooks work

The game's code is compiled with `-fpatchable-function-entry=16`: each of its functions starts with 16 bytes of
no-ops. A hook writes a jump to the mod's function over them, and the original is the code after them
(`port/platform/mod_hooks.c`). Only the game's own functions can be hooked (not Holyland's, not the C library), and
only where the compiler did not fold them into their callers: `hook` returns 0 then. Hooks are made in `hl_mod_init` or
at `HL_EVENT_START`.

### Building one

A plugin is built with Holyland, with the same compiler and the game's headers and flags. Configure Holyland with
`-DKF4_MODS_SRC=<a folder of mods>`: each mod there with `src/CMakeLists.txt` is built along with the game, into its
own `bin/<system>/`:

```cmake
# <mod>/src/CMakeLists.txt
holyland_add_mod(OUTPUT journal GAME RMLUI SDL
    SOURCES plugin.cpp journal.c journal_model.c journal_text.c journal_ui.cpp hooks.c)
```

`GAME`: its C files include the game's headers (`entity/entity.h` ...) and are built with the game's flags. `RMLUI`:
it uses the program's RmlUi. `SDL`: SDL3, for input events. The plugin links against the program's exports, so a
C++ plugin uses the program's RmlUi and C library, and must be built with the same toolchain (llvm-mingw on Windows).
`modding/examples/hello` is a small example: a key, a setting, events and a hook.

## Trust

A plugin is a program: it runs with the same rights as Holyland and can do anything Holyland can. Holyland does not
sandbox it. What Holyland does instead:

- A mod with a plugin starts **off**, and the Mods page says so under its name ("Contains code: it runs its own
  program inside Holyland. Turn on only mods you trust."). Data-only mods (no plugin) start on.
- Holyland never downloads mods; the player puts the folder in `mods/`.
- `--mods none` starts with no mods at all, data or code, to tell whether a problem is a mod's.
- The log names each plugin as it loads, each hook it makes, and each mod built for another Holyland release.

## Stability

- **API 1** keeps its meaning: later minor versions only add to the end of `HlApi`. A plugin that uses only the API
  works with every Holyland of API 1.
- **The game's own functions and structures** (`symbol`, hooks, the game's headers) are the decompilation's, and can
  change with any Holyland release. A plugin that uses them names the release in `"holyland"`, and is built again for a
  new one.

## Not done yet

- **Load order** is the folders' names, sorted; a mod cannot ask to load after another.
- **Save states** keep game RAM, so what a plugin put there with `game_alloc` is kept; its own variables are not, and
  there is no event when a state is loaded.
- **A plugin's files for a save** (`HlSave`'s key) are the plugin's to write and read: Holyland does not delete them
  with the save.
- **A crash in a plugin** ends the game like any crash. The crash report does not name the mod, and a plugin that
  crashes is not turned off for the next start.
- **Inlined functions** cannot be hooked (`hook` returns 0). Which ones a mod will want is learned as mods are
  written; such a function can be marked `noinline` in its patch, or given an event.
- **Scripting** (Lua) is not part of API 1; it could be added on top of the same API (see the pull request that added
  this).

## Files

| file | what |
| --- | --- |
| `modding/include/holyland_mod.h` | the API, for plugins |
| `modding/holyland_mod.cmake` | `holyland_add_mod`, building plugins with Holyland |
| `port/platform/mods.c` | the mod list (`mod.json`), on and off, settings; files and text (MODS.md) |
| `port/platform/mod_api.c` | loading plugins, the API, events, game memory |
| `port/platform/mod_hooks.c` | hooks |
| `game/mod_events.c` | the game's side of the events (`core.patch` calls it each frame) |
| `port/ui/mods_rows.cpp` | the Mods page |

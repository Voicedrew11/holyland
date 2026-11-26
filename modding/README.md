# Modding Holyland

- `include/holyland_mod.h`: the API a mod's plugin is built against.
- `hookable.txt`: the game's functions a plugin can hook.
- `game_interface.txt`: the game interface ID plugins are built against.
- `holyland_mod.cmake`: `holyland_add_mod`, to build plugins with Holyland (`-DKF4_MODS_SRC=<folder of mods>`).
- `examples/hello`: a small mod with code: a setting, a key, events and a hook on a game function.

The guide: [docs/port/MODDING.md](../docs/port/MODDING.md).

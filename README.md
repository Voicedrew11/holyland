# Darker Reality

A native PC port of **King's Field: The Ancient City** (*King's Field IV*; FromSoftware, 2001), built
from the [kfiv-decomp](https://github.com/Voicedrew11/kfiv-decomp) decompilation of the PlayStation 2
game (USA, SLUS-20318). The game's own code is compiled for Windows and Linux as a 64-bit program;
`port/` stands in for the PS2 under it.

> No game data is in this repository. When the port can load the game, you will need your own disc
> image.

**Status: step 0.** The game code compiles and links on Windows and Linux, and runs its start-up
until it asks for the disc. Nothing is drawn or heard yet. Next come the disc, the thread
scheduler and the pad (step 1: boot to a new game), then graphics and sound. See
[`docs/port/PLAN.md`](docs/port/PLAN.md) for the plan.

## Building

You need CMake 3.20 or newer, Ninja, and clang or gcc. The game code is GNU C, so MSVC's `cl` cannot
build it.

**Windows:** use [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), or LLVM's clang, with it on
`PATH`:

```sh
cmake --preset windows-clang
cmake --build out/windows-clang
out/windows-clang/kf4.exe
```

**Linux:**

```sh
cmake --preset linux-gcc          # or linux-clang
cmake --build out/linux-gcc
out/linux-gcc/kf4
```

For now the program prints the PS2 calls it reaches that the PC does not do yet (`[todo] ...`), then
waits for the disc. Stop it with Ctrl+C.

## Saving

Three ways to keep a game ([`docs/port/README.md`](docs/port/README.md) says more):
- **Quick saves (F2 to save, F4 to load): the ones to play with.** The game's own save, taken where you
  stand, without a save point. They load in any build of the port, so they survive updates. Loading
  one restarts the map from it, as loading a save does: enemies are back.
- **Save states (F5, F7): for debugging.** The whole running game, exact to the frame, but they load
  only in the build that made them, so an update makes them useless.
- **Saves**, at the game's save points: the game's three, each a file in the `saves` folder of the data
  folder (`save1.kf4save` to `save3.kf4save`; `--saves DIR` puts them elsewhere). A save is written whole
  or not at all, and the one it replaces is kept beside it as `.bak` (read instead if the save is damaged).

## Working on it

- [`docs/port/README.md`](docs/port/README.md): how the port is laid out, and how it stays in step
  with the decomp.
- [`docs/port/RULES.md`](docs/port/RULES.md): how the game code is changed for the PC (addresses
  in integers, file and save structs, inline asm, the SDK).
- `tools/patches.py`: `apply` the patches into `work/`, `save` edits back into `patches/`, and
  `upgrade` the decomp pin.
- `tools/port_census.py`: what still stops each folder of `src/game` compiling.
- `tools/port_abi_check.py`: functions declared with a different signature than they are defined
  with. These break on the PC even when they compile.

The decomp is a git submodule in `decomp/`, pinned to one commit, and the PC edits to it are patches
in `patches/`. Clone with `git clone --recurse-submodules`, or run `git submodule update --init`.

**The decomp is read-only in this repository.** Don't edit `decomp/` or the compiled copy in
`out/<preset>/decomp_src/` (where compiler errors point). Make game-code changes in `work/` with
`tools/patches.py apply`, then `tools/patches.py save` them into `patches/`. See
[`docs/port/RULES.md`](docs/port/RULES.md). Fixes to the reconstruction itself go to kfiv-decomp first. The
decomp's matching PS2 build (`build.sh`) is documented there.

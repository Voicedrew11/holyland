# The PC port

holyland is a native PC port of King's Field IV built from the decomp's source. The game code is the
decomp's, pinned in `decomp/` and never edited here; the PC edits to it are kept as patches in
`patches/`. Under it, `port/` stands in for the PlayStation 2. The approach follows
[ico-pc](https://github.com/nathanialf/ico-pc), the PC port of the ICO decomp:
- The game code is compiled for the PC as 64-bit C, with nothing emulated or recompiled.
- Where the code relies on the PS2, it is changed to the PC form, by the rules in
  [RULES.md](RULES.md).
- `port/` provides what the PS2 did, keeping the SDK's names so the game code reads as before:
  - game RAM;
  - the hardware registers;
  - the Sony libraries;
  - later the renderer, the sound driver and the platform layer.

## The decomp and the patches

`decomp/` is a git submodule of [kfiv-decomp](https://github.com/Voicedrew11/kfiv-decomp.git), pinned
to one commit (currently `88b0978`).

> **The decomp is read-only here.** Never edit `decomp/` or the compiled copy in
> `out/<preset>/decomp_src/`; this goes for people and agents alike. Configure fails if `decomp/` has
> local changes, and `decomp_src/` is made read-only (an edit there would be lost at the next
> configure). Edit game code in `work/` and save it into `patches/` (below).

Clone with submodules:

```sh
git clone --recurse-submodules <this repository>
# or, after a plain clone:
git submodule update --init
```

The PC edits are in `patches/`, as diffs against the decomp with paths relative to its root:
- `patches/game/<folder>.patch`: one per top-level folder of `src/game`.
- `patches/game/_top.patch`: the files directly in `src/game`.
- `patches/nonmatching.patch`: the map draw `src/nonmatching/func_00112BD0.c`, which the PC build
  compiles.

Game code that exists only on the PC, with nothing in the decomp to patch, is ordinary files in
`game/`: `game/data/` holds the data sections that the decomp still has in asm, defined in C. It is
compiled with the patched decomp and edited in place.

The decomp's own tooling and docs (its PS2 build, `build.sh`, `tools/match.py`, the splat config) live
in kfiv-decomp. They do not apply to the patched game code here.

Fixes that are not PC-specific (names, types, structs, matching) go to kfiv-decomp first, and come
here with a pin move (see below).

### Changing game code

1. `python3 tools/patches.py apply`: makes `work/` (git-ignored) a fresh copy of the decomp with the
   patches applied. `work/` is its own git repository, and its `pristine` tag is the unpatched decomp.
   If `work/` has changes that were not saved, `apply` refuses; `apply --force` discards them.
2. Edit the game code in `work/src/game`, by the rules in [RULES.md](RULES.md).
3. `python3 tools/patches.py save`: regenerates `patches/` from `work/`. Commit `patches/`.
4. `cmake --build out/<preset>`: since the patches changed, configure runs again on its own. It
   copies `decomp/src/game` and `decomp/src/nonmatching`, applies `patches/` to the copy, and updates
   `out/<preset>/decomp_src/` with the files that changed, so only those recompile. The build compiles
   `decomp_src/` with `game/` and `port/`. Configure fails if `decomp/` has local changes.

### Moving the pin

`python3 tools/patches.py upgrade <commit>` moves the submodule to a newer kfiv-decomp commit:
- it follows file renames (`git diff -M` between the two pins) and function and global renames (from
  `config/symbol_addrs.txt`, whose addresses are stable);
- it re-applies the patches with `git apply -3` and reports conflicts.

It does not follow renames of struct fields or types: `symbol_addrs.txt` has only functions and
globals. Those come up as conflicts, or as compile errors after a clean upgrade, and are fixed by
hand.

If that is clean, it moves the submodule and rewrites `patches/`. Then commit `decomp` and `patches/`.
If not, it keeps the scratch copy so the conflicts can be resolved there, and then
`python3 tools/patches.py save --from <that dir>` writes the result to `patches/`.

## Building

The game code is GNU C, so build it with clang or gcc (not MSVC's cl). You need CMake 3.20 or newer
and Ninja.

**Windows:** use [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), or LLVM's clang, on the PATH:

```sh
cmake --preset windows-clang
cmake --build out/windows-clang
```

**Linux:**

```sh
cmake --preset linux-gcc        # or linux-clang
cmake --build out/linux-gcc
```

The program is `out/<preset>/kf4`. SDL2 is used for the pad if CMake finds it (`-DKF4_USE_SDL=OFF`
turns it off).

## Running

```sh
out/linux-gcc/kf4 --disc kfiv_us.iso
```

| Option | What |
|---|---|
| `--disc PATH` | the game's disc image (or set `KF4_DISC`) |
| `--fast` | run as fast as possible instead of at 60 vblanks a second |
| `--headless` | no window, no SDL |
| `--frames N` | stop after N vblanks |
| `--input FILE` | play the pad from a script instead of SDL |
| `--log FILE` | the per-frame log (`-` for stdout) |
| `--clock YYYY-MM-DDTHH:MM:SS` | the disc drive's clock at boot, in Japan's time, instead of the host's; for repeatable runs |
| `--host-dir DIR` | the folder `host0:` paths are read from |

A pad script has one press per line: `<vblank> <buttons> [vblanks held, default 6]`. The buttons are
names such as `CROSS`, `START`, `UP`, joined with `+`, or `-` for none. A line `<vblank> lx=N ...` (or `ly`, `rx`,
`ry`) sets a stick (0x80 is the middle). Text from `#` on is a comment. `tests/newgame.pad` boots to
a new game.

The log has one line per vblank event (`<vblank> event: ...`: files read, movies, the memory card),
and one per game frame with the game's status, the map and the player's position.

`tools/boot_test.py --disc kfiv_us.iso` is step 1's check: it boots to a new game twice and checks
the two logs match and reach the first map.

Each folder of the game code is its own target, so `cmake --build out/windows-clang --target game_ui`
compiles just the `ui` folder (`src/game/ui`, with the patches applied).

## Status

The steps, in order: [PLAN.md](PLAN.md).

**Step 0** (done): the game code compiles and links as a 64-bit program on Windows and Linux,
and `main` runs until it needs something the stand-ins do not do yet.

- `tools/port_census.py` counts, per folder, the errors and the inline asm still standing between
  each folder and that goal.
- `tools/port_census.py --show ui/menu_draw.c` prints one file's errors.
- `tools/port_census.py --show ui` prints a folder's.

**Step 1** (done): the stand-ins (thread scheduler, files, DMA, IOP, memory card, pad) get the game
from boot to a new game with nothing drawn, deterministically (`tools/boot_test.py`).

Next:
1. **Graphics:**
   - first the game's packets go to paraLLEl-GS;
   - then the VU1 programs become native renderers.
2. **Sound.**

## Layout

| Path | What |
|---|---|
| `decomp/` | submodule: kfiv-decomp at the pinned commit. Never edited here |
| `patches/` | the PC edits to `decomp/`, as diffs (`game/<folder>.patch`, `game/_top.patch`, `nonmatching.patch`) |
| `game/` | PC-only game code, compiled with the patched decomp (`game/data/`: the data sections in C; `state_log.c`: the per-frame log) |
| `work/` | git-ignored: the decomp with the patches applied, for editing (`tools/patches.py apply`) |
| `port/include/` | the SDK headers the game includes (`libgraph.h`, `eekernel.h` …) for the PC, and `port/host.h` |
| `port/platform/` | the program's entry and options, game RAM, EE addresses, the register pages, the EE kernel (fibers, virtual time), the DMAC, the disc |
| `port/sdk/` | the Sony libraries' PC versions: files and the IOP over SIF, the sound driver's RPC, libcdvd, libdma, libpad, libmc; stand-ins for libgraph, libmpeg and libsdr |
| `port/math/` | libvu0 in C |
| `tools/` | `patches.py`, `port_census.py`, `port_abi_check.py`, `boot_test.py` |
| `tests/` | pad scripts for headless runs (`newgame.pad`) |
| `.github/workflows/ci.yml` | CI: configure (the patches apply), build, `port_census.py` reports 0 errors |
| `CMakeLists.txt`, `CMakePresets.json` | the build; it compiles the patched game code from `out/<preset>/decomp_src/`, with `game/` |
| `docs/port/` | these notes |

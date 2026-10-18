# The plan for the PC port

holyland is a native PC port built from the kfiv-decomp source. This page lists the steps in
order: what each one delivers, and how we check it. Each step ends in something we can run and
compare against the PS2.

The approach follows [ico-pc](https://github.com/nathanialf/ico-pc), the PC port of the ICO decomp:
- the game's own C is compiled for the PC, with no emulator or recompiler;
- `port/` stands in for the PS2 under it;
- the PC behaves like the PS2 by default, and every improvement is opt-in.

Windows and Linux are both first-class. Every step has to build and run on both.

## Step 0: native 64-bit build (done, branch `port/step0`)

The game code compiles and links as a 64-bit program with clang on Windows and gcc or clang on
Linux. It runs its start-up (system, display, sound driver, timers) until it asks for the disc.

What changed:
- The game code is converted to its PC form by the rules in [RULES.md](RULES.md):
  - addresses kept in integers;
  - structs read from files or saves keep their PS2 layout, and the others may grow;
  - inline asm and VU0 macro code are rewritten in C;
  - SDK calls go through PC headers.
- About 680 data symbols that only existed in the asm data files are now defined in C, in
  `game/data/`.
- Four independent reviews of the whole conversion found about 35 bugs the compiler cannot see.
  All are fixed. The worst were:
  - calls that only worked because the EE leaves unused arguments in registers;
  - local prototypes that pass arguments in the wrong positions on Windows;
  - globals that were one object on the PS2 but separate objects on the PC;
  - structs that outgrew their PS2-sized buffers.
- Two tools keep these from coming back:
  - `tools/port_census.py` counts what stops each folder compiling;
  - `tools/port_abi_check.py` finds declarations whose calling convention differs from the
    definition.

Still open:
- No tree-wide check yet for integer division by zero (x86 traps, the EE does not).
- Model work areas are sized for the PS2's worst case, not for the real part counts.
- PS2 float rounding is not modelled beyond the division and square-root edge cases.

## Step 1: boot to a new game, headless (done, branch `port/step1`)

Get from boot to the first map of a new game with nothing drawn, by making the stand-ins in
`port/sdk` real enough:

| Area | What it needs to do |
|---|---|
| Disc and files | `sceCd*` and `sceOpen`/`sceRead`, served from the extracted disc folder or the ISO, so `KF4.DAT`, the font, the menus and the maps load. |
| Threads and timing | Fibers with the EE kernel's rules: strict priority, switching only in kernel calls. Includes the game's two threads, semaphores, the vblank interrupt handler, `sceGsSyncV` pacing and timer 0. |
| DMA and the GS | DMA channels that finish, so the game's wait loops on `D1_CHCR`/`D2_CHCR`/`GIF_STAT` end. `sceDmaGetChan` has to use the same registers as `eeregs.h`. |
| IOP | Module loads and the IOP heap work; the sound driver's RPC accepts every command and reports done. |
| Memory card | "No card inserted". |
| Movies | Each movie ends at once. |
| Pad | SDL, plus scripted input for tests. |

**Check:** a headless run that is deterministic (the same input gives the same run) and logs the
game's state each frame: game status, current map and player position. It must show title →
New Game → first map. The same run then checks every later change.

Also to verify here: map scripts read and write the player, game-state and view-ray blocks by PS2
byte offsets. These are now translated to the PC fields (`Script_GameVarAt`).

Result:
- `tools/boot_test.py` runs the boot twice with `tests/newgame.pad` and checks it: the title loads at
  vblank 122, New Game's opening movie ends at 714, `099.map` loads at 716, and from frame 1 the
  player stands in map 0x63 with 50 HP. The opening walk runs, and the player has control at frame
  370. The two runs give the same log, and so do gcc and clang. 6000 vblanks take about 0.4 s.
- Every map's script was scanned for game-variable operands. The game-state block is used only
  below 0x40 and the player block only below 0x200, which are the same offsets on both. The
  view-ray block is never used. So no retail script reaches a translated field, and the translation
  is a safeguard only.
- Three game bugs the PC exposed, fixed in `patches/`:
  - `File_Read` reads whole sectors, so a file read into a static table runs past its end. Retail
    lays the tables out so that is harmless. The save icon buffer is enlarged to whole sectors.
    GCC builds with `-fno-toplevel-reorder` so globals keep the order they are defined in.
  - An effect with no magic reads `fx_table[0xFFFF]`, 17 MB past the table: that is game RAM on
    the PS2, and is read there (`EE_RAM_AT`).
  - `ModelInstance.found_record` takes an aligned 128-bit store, so it is 16-byte aligned.

Still open:
- Movies are not played: each one ends at once (`sceMpeg*`).
- The memory card is always empty, so saving and loading are untested.
- The sound driver's RPC answers every command with "done" and plays nothing.

## Step 2: graphics, stage 1 (the first picture)

Hand the game's GS packets to paraLLEl-GS, the accurate GS rasteriser. The packets come from:
- one VIF1 chain per frame, sent from `gfx/packet_buffer.c` through the four VU1 programs;
- the menus' GIF packets on channel 2.

The VU1 programs run on the CPU for now. This gives an accurate picture quickly, and it becomes
the reference the native renderer is compared against.

## Step 3: graphics, stage 2 (native renderer)

A renderer under the game's draw layer, as ico-pc has:
- The four VU1 programs become shaders that read the game's own vertex data. Each has a CPU
  version for testing.
- The GS states the game actually uses become pipelines.
- Texture and frame-buffer addresses map to textures and render targets. This includes the feedback
  effects: the screen wobble, and the frame copied into a texture at TBP 0x1180.
- Each piece is checked frame by frame against stage 1.

Then, all opt-in:
- higher resolution;
- widescreen (we own the projection code);
- higher frame rates, with interpolation while the game itself keeps its 30 Hz.

## Step 4: sound

One of two ways:
- Run FromSoftware's IOP sound driver (FSIOPSND) under an IOP emulator with an SPU2 model, as the
  old recomp-based holyland did. This is quicker.
- Rewrite the driver on the PC: its command switch (`fsSndrFunc`), the BD/HD/SQ sequencer and a
  software SPU2. This is cleaner, and it is what ico-pc did for its driver.

## Step 5: saves and movies

- The memory card becomes a folder.
- Movies are decoded with libmpeg2 or FFmpeg from the PSS files, keeping the PS2's timing.

## Step 6: PC features and releases

- Features: a settings menu, rebinding, mouse look.
- First run extracts the game from the user's ISO.
- Builds and releases come from CI.
- Before binaries are shipped, the game's data tables are loaded from the user's own ELF instead of
  being compiled in, as ico-pc does.

## Later (optional)

As OpenGOAL did, some things could be drawn natively straight from the game's structures, without
the GS path (maps and models first).

## Working together

- The game code comes from kfiv-decomp, a git submodule in `decomp/`, pinned to one commit. It is
  never edited here. Clone with `--recurse-submodules`, or run `git submodule update --init`.
- The PC edits are patches against the decomp in `patches/`. To change game code, run
  `python3 tools/patches.py apply`, edit in `work/src/game`, then `python3 tools/patches.py save` and
  commit `patches/`. PC-only game code, such as `game/data/`, is edited in place.
- Fixes to the reconstruction itself (names, types, structs) go to kfiv-decomp first.
- To move the pin, run `python3 tools/patches.py upgrade <commit>`. It follows file, function and
  global renames (not struct-field or type renames) and re-applies the patches. Commit `decomp` and
  `patches/` if it is clean, or resolve the conflicts in the scratch copy and run
  `patches.py save --from <that dir>`.

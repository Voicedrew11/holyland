# Turning the game code into its PC form

The rules for editing the game code (`src/game`) so it compiles and runs as a 64-bit PC program.
Read `port/host.h` alongside: it defines everything these rules use.

## Where to edit: the decomp is read-only

The decomp is read-only in this repository. Never edit:
- `decomp/`, the kfiv-decomp submodule. Configure fails if it has local changes.
- `out/<preset>/decomp_src/`, the patched copy that is compiled. Compiler errors point into it, but
  its files are read-only, and the next configure would overwrite an edit there anyway.

Edit the game code only in a work copy:

```sh
python3 tools/patches.py apply   # work/ = the pinned decomp with patches/ applied
# ... edit work/src/game ...
python3 tools/patches.py save    # writes the edits back into patches/; commit patches/
```

To find the file a compiler error names, swap `out/<preset>/decomp_src/` for `work/`; the paths below
it are the same. `game/` (PC-only game code) and `port/` are ordinary files, edited in place.

A fix to the reconstruction itself (a name, a type, a struct) is not a PC edit: it goes to kfiv-decomp,
and comes here when the pin moves.

## The goal, and what not to change

**The PC must do what the PS2 did.** Keep every behaviour, including retail bugs: don't fix the game,
don't "improve" it, don't rename things. A retail bug found along the way goes in a comment and in
your report.

**Change as little text as you can.** The patches are carried onto every new decomp commit
(`patches.py upgrade`), and every changed line is a possible conflict.
- No reformatting.
- No moving code.
- Don't rename functions or variables.

**Matching artifacts may go.** The decomp wrote some code oddly to reproduce ee-gcc's output. Where
such code is what stops the PC build, write it plainly, keeping what it does. That covers:
- casts that only steer register allocation;
- `(int)ptr` arguments to functions that take pointers;
- `// match:` tricks.

Delete a `// match:` comment when the line it explains is gone.

## 1. Integer types

- `s64`/`u64` are `int64_t`/`uint64_t`. **Never write `long`:** on Windows it is 32-bit, while on the
  EE it was 64.
  - Replace `long`, `unsigned long` and `long long` with `s64`/`u64`.
  - Replace an `L` suffix on a 64-bit constant with `LL`, or with a cast to `s64`/`u64`.
- `Uint128` is `unsigned __int128`.

## 2. Addresses kept in integers

On the EE a pointer is 32 bits, so the game keeps addresses in `int`/`s32`/`u32`. On the PC an
address is 64 bits. The build makes every place that squeezes one into 32 bits an error:
- `-Werror=pointer-to-int-cast`
- `-Werror=int-to-pointer-cast`
- `-Werror=int-conversion`
- `-Werror=shorten-64-to-32` (clang)

Fix each by what the integer is.

**a. It only ever holds an address, and nothing outside the program sees its bits.** That covers a
local, a parameter, a return value, a global, or a field of a runtime struct (see 3).
- If its uses allow, make it a real pointer of the right type. Best.
- Otherwise make it `sptr` (signed) or `uptr` (unsigned): integers as wide as a pointer.

When you change the type of a global, a field or a function's parameter or return, change **every
declaration of it in the whole tree** (the decomp declares globals with `extern` in many `.c` files),
and list it in your report. A declaration left at 32 bits in another file compiles silently and reads
half the value.

**b. Its 32 bits are fixed.** It's a field of an overlay or save struct (see 3), a word inside a file
loaded from disc, or an address word inside a DMA, VIF or GIF packet.
- Keep it 32 bits, as `EEWord`.
- `EE_WORD(ptr)` turns a pointer into one.
- `EE_PTR(T *, word)` turns one back.

EE addresses exist for game RAM and for the game's globals. A pointer to the stack or the C heap has
none, and `EE_WORD` stops the program if it gets one. Write the pointed-to type in a comment:
`EEWord parts; /* (ModelPart *) */`.

**c. Files that hold offsets, turned into pointers in place.** For example `table[i] += (int)table;`
or `header->x = (int)header + header->x;`.
- The words stay `EEWord`, and the base is `EE_WORD(table)`.
- Everything that reads them afterwards uses `EE_PTR`.
- Files are loaded into game RAM (the arena in `core/memory.c`), so their pointers have EE
  addresses.

**d. It is really a number.** That covers a size, an index, or a difference of two pointers.
- Cast it explicitly: `(s32)(end - start)`.

**Lvalue casts** (`(int)p += 4`, `((u8 *)p)++`) are not C any more. Write them out:
`p = (T *)((u8 *)p + 4)`.

## 3. Structs: runtime, overlay, save

Every struct is one of three kinds.

**Runtime.** Only the program makes and reads it. Its layout is free:
- pointer fields grow to 8 bytes;
- fields that hold addresses become pointers or `sptr` (2a).

Most structs are runtime.

**Overlay.** It lies over bytes the program did not lay out, so its layout must stay the PS2's
exactly. That covers:
- data loaded from disc (a model, map, motion, param or texture file, or any part of one);
- the GS/VIF/DMA packet formats;
- anything a pointer into a file buffer is cast to.

Rules for overlay structs:
- No real pointers in them: a pointer field becomes `EEWord` (2b).
- Add layout checks after the definition, from its offset comments:
  `LAYOUT_SIZE(T, 0x40); LAYOUT_OFFSET(T, field, 0x10);`
- Mark it with `/* overlay: <what it lies over> */` above the definition.

**Save.** It is written to or read from a memory card save, so its layout is that of the save file.
Treat it as an overlay, marked `/* save: ... */`.

How to tell overlay from runtime: follow where its memory comes from. Signs of an overlay:
- a cast of a file buffer, `map_file_area` or another area a file is read into;
- `(T *)((u8 *)header + header->offset)`;
- a struct whose offsets are relocated with `+= base`;
- `sizeof(T)` used in a file read.

If the same memory is viewed both ways, the overlay rule wins.

**Code that assumed a runtime struct's PS2 size or offsets.** Once pointer fields grow, these break:
- hard-coded byte offsets into a runtime struct (`*(s32 *)((u8 *)p + 0x40)`, `MAP_OBJECT_BYTES`-style
  macros) become field accesses;
- word counts that copied a whole struct (`Memory_CopyWords(dst, src, 0x40)`) become
  `sizeof(T) / 4`;
- a fixed-size area of game RAM that holds runtime structs may now overflow. Leave the size, and add
  `/* PORT: holds runtime structs, may need more room on the PC */`.

**Reads past the end of a buffer.** `File_Read` reads whole sectors (0x800 bytes), so a file read
into a buffer runs up to 0x7FF bytes past its end. Retail lays its buffers out so the overrun lands
on something harmless. On the PC that order is not kept, so make the buffer whole sectors and say so
in a `/* PORT: */` comment. In the same way, an index past the end of a table read game RAM at the
EE address on the PS2: read it there with `EE_RAM_AT`.

**128-bit stores.** Code that stores a `Uint128` or `VuIntVector` into a runtime struct needs the field
16-byte aligned. The PS2 layout put such fields on a 16-byte boundary, and the PC layout may not: give
the field an aligned type.

## 4. Fixed addresses

| PS2 | PC |
|---|---|
| scratchpad `(T *)0x70000000 + n` | `SPR(T *, n)` |
| hardware register `*(volatile int *)0x1000A000` | the `eeregs.h` name (`*D2_CHCR`), or `*EE_HW(volatile int *, 0x1000A000)` |
| uncached `p \| 0x20000000`, uncached-accelerated `\| 0x30000000`, `& 0x0FFFFFFF` | `UNCACHED(p)`, `UNCACHED_ACCEL(p)`, `PHYS(p)` |
| a fixed address in game RAM | `EE_RAM_AT(T *, 0x...)` |

Loops that wait on a hardware register stay as they are. Step 1 makes the host set the bits they wait
for. Add `PORT_TODO("waits for <register> <bit>")` before any loop you see that would spin forever on
a register that nothing sets.

## 5. Inline asm

No game file may keep `asm`/`__asm__`. Rewrite each block as C that computes the same thing.

- **Floating point:** keep the order of operations (`a*b + c` stays one multiply then one add, not
  regrouped). The build already stops the compiler fusing them.
- **VU0 macro mode** (`lqc2`, `vmul`, `vmadd`, `vmula`, `vdiv`, `vsqrt`, `vopmula` …):
  - Use plain float arrays when the values live only inside the block.
  - Use `ee_vu0.vf[n][0..3]`, `ee_vu0.acc`, `ee_vu0.q` and `ee_vu0.vi[n]` when the game leaves values
    in VU0 registers for a later block or call to use. `math/vu0_macro.c` keeps matrices in vf1..vf14
    across calls.
- **Field masks:** `.xyz`, `.w` and the broadcast suffixes (`x`, `y`, `z`, `w` on `vmulx` …) are
  exact: write each component.
- **EE integer and 128-bit instructions** (`lq`, `sq`, `pextlw`, `pcpyld`, `ppacw`, `qfsrv`, `mult1`
  …): write them in C on `u32[4]`, `Uint128` or `u64`.
- **`sync`, `ei`, `di`, cache instructions:** delete them.
- **`mfc0 $count` (the cycle counter):** `PORT_TODO` and return 0 for now.

## 6. The Sony libraries

The game calls the SDK through the headers in `port/include`, named as on the PS2:
`eekernel.h`, `eeregs.h`, `libgraph.h`, `libdma.h`, `libvu0.h`, `libpad.h`, `libmc.h`, `libmpeg.h`,
`libcdvd.h`, `sifdev.h`, `sifrpc.h`, `libsdr.h` and `libdev.h`.
- A file that calls an SDK function includes its header, and deletes its own prototype of that
  function. The decomp often declared them locally, with the types the matching needed.
- When the call does not fit the header's signature, change the call. Most mismatches are an address
  passed as an int. If the header is wrong, say so in your report.
- Things that pass the SDK an address as a number now pass a pointer:
  - `sceDmaSend(channel, packet)`;
  - the client data of `sceSifBindRpc` and `sceSifCallRpc`, which is a `sceSifClientData`, not an int
    array.

On the PC the libraries are stand-ins for now (`port/sdk/stubs.c`). Step 0 only needs the calls to
compile and link.

## 7. Data the decomp still has in asm

About 800 small globals are not yet defined in C on the PS2 side; their definitions are in splat's
data files. One agent defines them all in C, under `game/data/`. When your folder declares one of
them `extern`, keep the declaration (fixing its type by rule 2), and don't define it yourself.

## 8. Checking your work

```sh
cmake --preset windows-clang                      # once (or linux-gcc)
cmake --build out/windows-clang --target game_ui  # your folder
python tools/port_census.py                       # every folder's errors and asm left
python tools/port_census.py --show ui             # one folder's messages
python tools/port_abi_check.py                    # declarations that disagree with the definition
python3 tools/patches.py save                     # your edits in work/ into patches/
```

A folder is done when:
- its census row is 0 errors and 0 asm;
- `port_abi_check.py` reports nothing for names you touched;
- your edits are saved into `patches/`, and nothing under `decomp/` or `decomp_src/` was edited;
- you have reported every changed global, struct, signature or header.

# Sanitizers

`-DKF4_SANITIZE=...` builds everything (the game, `port/`, the tests) with clang's sanitizers. Use clang: the gcc
sanitizer builds don't link. `port/include/port/sanitize.h` holds what the port's own tricks need under them (the
fibers' stack switches, save states' raw copies).

## ASan + UBSan

```sh
cmake --preset linux-clang -B out/linux-clang-asan -DKF4_SANITIZE=address,undefined
cmake --build out/linux-clang-asan
```

Known reports, not the port's (docs/port/PC_RENDERER.md): `SoundDriver_Remote`, and the decomp's calls through
function pointers cast to another type.

## ThreadSanitizer

```sh
cmake --preset linux-clang -B out/linux-clang-tsan -DKF4_SANITIZE=thread
cmake --build out/linux-clang-tsan
TSAN_OPTIONS="suppressions=$PWD/tools/tsan.supp halt_on_error=0 second_deadlock_stack=1 report_signal_unsafe=0 log_path=/tmp/tsan" \
  KF4_DATA_DIR=/tmp/kf4data setarch $(uname -m) -R out/linux-clang-tsan/kf4 --disc <image> --renderer pc --headless \
  --no-audio --memcard none --input tests/newgame.pad --teleport 1900:9:0 --invincible --frame-rate 240 \
  --frames 2300 --dump-frames 2299 --dump-dir /tmp/kf4dump --stats
```

- `setarch -R` (no address randomisation): with a newer kernel's `vm.mmap_rnd_bits` TSan stops at start with
  "unexpected memory mapping".
- The pictures between, and so the pump (`kf4-pc-pump`), run only above 60: `--frame-rate 240`. Headless,
  `--frame-rate unlocked` draws almost none; with `KF4_PRESENT_SIM=240` it does, and the presenter's thread
  (`kf4-between`) runs with its pacing. `KF4_PRESENT_SIM` needs a build with SDL2, and runs at the wall clock's pace.
- On lavapipe a run takes about twice as long as with a normal build (the GPU's work, done on the CPU by Mesa, is not
  instrumented).
- Without a GPU (a VM, WSL): lavapipe, `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`. Two things to know:
  lavapipe (Mesa 25.2) crashes in `vkUpdateDescriptorSetWithTemplate` on the depth/stencil surface read as an input
  attachment (DATE by the stencil), so run with `KF4_PC_STENCIL=0` (DATE by a copy; the threads are the same). And it
  can take a lot of memory: cap the run (`systemd-run --user --scope -p MemoryMax=8G ...`), one at a time.
- `--no-audio` leaves the sound thread out.

### The suppressions (`tools/tsan.supp`)

Only Mesa's Vulkan drivers (`libvulkan_lvp.so`, `libvulkan_radeon.so`): they are not built with TSan, which sees
their memcpy, malloc and pthread calls but not the atomics and futex locks that order them. Seen: image views made by
kf4-pc and the pump at once (the canvases at start), lavapipe's fences freed, a hash table on the llvmpipe threads.
A `race:` entry hides a report when any frame of either stack matches, so a race between our code and a Mesa thread
reading our memory would be hidden too; on a real GPU TSan never sees that work at all.

### What the port does for it

- The DMA channels' CHCR registers are `_Atomic` (`EeChcr`, `port/include/eeregs.h`): the game's store of STR
  publishes its packet to the DMA controller's thread, and the controller's clear of STR hands it back. They were
  plain volatile stores and loads, and nearly every transfer was a report (about 260 of a run's 280).
- Each fiber (the EE threads, all on the host's main thread) has a TSan context of its own (`fiber.c`), switched with
  a happens-before, so a report's stack is the EE thread's own. Reports name a fiber as a thread "created by main
  thread at port_fiber_create".
- Save states are off in the TSan build: TSan's memory layout leaves no room for game RAM at its fixed address
  ("game RAM is not at its fixed address: no save states"). Quick saves work.

### Where it stands (2026-11-11)

Run on lavapipe in WSL at 000, 009, 009:2 and 063 at 240 (and 000, 009, 063 with `KF4_PRESENT_SIM=240`), 009:2
unlocked with `KF4_PRESENT_SIM=240`, and a quick save at 009 loaded again (a map load): with the suppressions, no
reports. Fixed on the way:
- the CHCR hand-over above;
- `sceGsPutDrawEnv` sent the game's draw environment in place, which the game rewrites every frame without waiting for
  the channel (the fog's clear colour, `Map_DrawRecords`): with the controller's thread behind, a transfer could read
  the next frame's. Now copied at the start;
- the presenter thread read `times_set` without its lock, `between_thread_up` before `between_start` set it, and
  `between_stop` as a plain volatile: atomics now.

The fixes change no picture: against main at 000 (with the title and menus), 009:2, 063 and the quick save loaded at
240 (`KF4_INTERP_DRAW_ALL=1`, Windows, 1x), 118 pictures byte for byte.

Not covered headless: the window and the overlay (`kf4-between` reads `port_overlay_visible` and the mouse indicator's
time, written by the game's thread), the flash recorder, the sound thread, RADV, a 4x scale.

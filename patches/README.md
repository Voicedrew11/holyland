# patches/

Patches applied to [PS2Recomp](https://github.com/ran-j/PS2Recomp) by
`scripts/03-build-runner.sh`, in filename order, on top of the commit pinned
as `PS2X_REF` in `scripts/common.sh`. They are generated with
`scripts/maintainer/export-patches.sh` from a PS2Recomp branch (one fix per
commit); don't edit them by hand.

| Patch | Why |
|---|---|
| `0001-kfiv-dev-harness-…` | Opt-in dev harness (env vars, off by default): PNG frame dumps, scripted pad input, a command file for live control, hidden window, auto-exit. See `docs/maintainers.md`. |
| `0002-gs-keep-GIFtag-state-across-transfers-per-PATH` | The GIF unit keeps its tag state between DMA transfers. libgraph's `sceGsExecLoadImage` sends the A+D/IMAGE header and the pixels as two DMAs; the runtime restarted parsing at every packet, so pixel data was parsed as GIF tags (garbled title screen, hangs). Also: `NLOOP=0` is an empty tag, `FLG=3` acts as IMAGE. |
| `0003-Add-opt-in-GS-diagnostics-…` | `PS2X_GS_TRACE` (register/transfer/primitive trace) and `PS2X_VRAM_DUMP` (raw 4 MB GS memory at exit). |
| `0004-gs-apply-DISPLAY.MAGV-…` | The presented height ignored `DISPLAY.MAGV`. |
| `0005-gs-interlaced-FRAME-mode-…` | With `SMODE2.INT=1, FFMD=1` each field reads `(DH+1)/2` lines (the bottom half of the window showed the Z buffer). The two field buffers are woven into a full 640×448 frame on the host (`PS2X_NO_WEAVE=1` keeps the raw field). |
| `0006-iop-first-fit-heap-…` | IOP heap: first-fit over 0x80000–0x1F0000 so freed blocks are reused (the movie player's 160 KB stream buffer failed to allocate). IOP `ioman` open/close/read/write/lseek/ioctl backed by the unpacked disc (needed by `FSIOPSND.IRX`). |
| `0007-Audit-and-fix-VIF1-VU1-emulation-…` | VIF1 UNPACK V2/V3/V4-5 component layout, STMOD mode 3, MPG address masking; VU1 EFU opcode table, RSQRT 0/0, XGKICK address wrap. |
| `0008-dev-harness-EE-thread-semaphore-dump-…` | Dev harness: `PS2X_THREADS_AT_EXIT=1` / ctrl `threads <file>` dump EE threads, semaphores and event flags (finds what a stuck game waits on). |
| `0009-Show-guest-printf-output-…` | The game's own `printf` diagnostics (e.g. `SifAllocIopHeap Error`) were only printed under aggressive logging; IOP `printf` printed its raw format string. |
| `0010-Run-interrupt-callback-handlers-on-stacks-in-EE-kern…` | Interrupt/callback handler stacks were carved from the top of RAM, on top of the main thread's stack, so every VBlank/DMAC handler overwrote saved registers (crash to `0x430000` when the opening movie starts). They now live in kernel RAM (0x20000–0x80000). |

PS2Recomp is GPL-3.0, so these patches are derivative works under the same
license. The rest of this repository is MIT (see `../LICENSE`).

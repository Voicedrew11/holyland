# patches/

Patches applied to [PS2Recomp](https://github.com/ran-j/PS2Recomp) by
`scripts/03-build-runner.sh`, in filename order, on top of the commit pinned
as `PS2X_REF` in `scripts/common.sh`. They are generated with
`scripts/maintainer/export-patches.sh` from a PS2Recomp branch (one fix per
commit); don't edit them by hand. Workflow:
[`docs/contributing.md`](../docs/contributing.md).

| Patch | Why |
|---|---|
| `0001-kfiv-dev-harness-…` | Opt-in dev harness (env vars, off by default): PNG frame dumps, scripted pad input, a command file for live control, hidden window, auto-exit. See `docs/contributing.md`. |
| `0002-gs-keep-GIFtag-state-across-transfers-per-PATH` | The GIF unit keeps its tag state between DMA transfers. libgraph's `sceGsExecLoadImage` sends the A+D/IMAGE header and the pixels as two DMAs; the runtime restarted parsing at every packet, so pixel data was parsed as GIF tags (garbled title screen, hangs). Also: `NLOOP=0` is an empty tag, `FLG=3` acts as IMAGE. |
| `0003-Add-opt-in-GS-diagnostics-…` | `PS2X_GS_TRACE` (register/transfer/primitive trace) and `PS2X_VRAM_DUMP` (raw 4 MB GS memory at exit). |
| `0004-gs-apply-DISPLAY.MAGV-…` | The presented height ignored `DISPLAY.MAGV`. |
| `0005-gs-interlaced-FRAME-mode-…` | With `SMODE2.INT=1, FFMD=1` each field reads `(DH+1)/2` lines (the bottom half of the window showed the Z buffer). The two field buffers are woven into a full 640×448 frame on the host (`PS2X_NO_WEAVE=1` keeps the raw field). |
| `0006-iop-first-fit-heap-…` | IOP heap: first-fit over 0x80000–0x1F0000 so freed blocks are reused (the movie player's 160 KB stream buffer failed to allocate). IOP `ioman` open/close/read/write/lseek/ioctl backed by the unpacked disc (needed by `FSIOPSND.IRX`). |
| `0007-Audit-and-fix-VIF1-VU1-emulation-…` | VIF1 UNPACK V2/V3/V4-5 component layout, STMOD mode 3, MPG address masking; VU1 EFU opcode table, RSQRT 0/0, XGKICK address wrap. |
| `0008-dev-harness-EE-thread-semaphore-dump-…` | Dev harness: `PS2X_THREADS_AT_EXIT=1` / ctrl `threads <file>` dump EE threads, semaphores and event flags (finds what a stuck game waits on). |
| `0009-Show-guest-printf-output-…` | The game's own `printf` diagnostics (e.g. `SifAllocIopHeap Error`) were only printed under aggressive logging; IOP `printf` printed its raw format string. |
| `0010-Run-interrupt-callback-handlers-on-stacks-in-EE-kern…` | Interrupt/callback handler stacks were carved from the top of RAM, on top of the main thread's stack, so every VBlank/DMAC handler overwrote saved registers (crash to `0x430000` when the opening movie starts). They now live in kernel RAM (0x20000–0x80000). |
| `0011-dev-harness-PS2X_STATS-…` | Dev harness: `PS2X_STATS=1` prints VSync ticks/s and display flips/s (the game's frame rate) once per second. |
| `0012-vu1-cut-interpreter-overhead-…` | VU1 interpreter ~2.3x faster with the same cycle and flag model: precomputed hazard slots, immediate write-back for interlocked VF/VI/ACC registers, bitmask pipelines, cheaper exact FMAC flag checks. Verified identical to the previous interpreter on every microprogram of a KFIV session. |
| `0013-gs-rasterize-asynchronously-on-a-pool-of-worker-thre…` | The GS CPU backend rasterizes on worker threads (`PS2X_GS_THREADS`, default min(8, cores/2)) while the EE keeps running, each worker owning interleaved screen stripes. Page-level hazard tracking, per-draw FP rounding mode. Verified pixel-identical to the previous backend on a recorded session. |
| `0014-gs-opt-in-recorder-of-raster-backend-calls-…` | `PS2X_GS_RECORD=<file>` records every GS backend call, for replaying through two backends with `scripts/maintainer/gsreplay`. |
| `0015-Allow-large-generated-runner-objects-…` | MSVC `/bigobj /MP` for the generated sources and a 16 MiB runner stack; the default Windows stack overflowed during initialization. |
| `0016-Add-Verdite-style-keyboard-and-native-mouse-look-…` | Verdite keyboard mappings, menu input, focus-aware mouse capture, and a USA-ELF-guarded native look override. |
| `0017-Bind-KFIV-IOP-heap-frees-…` | Bind the USA game's SDK heap-free entry to the existing IOP allocator so opening and closing inventory can reuse its buffers. |
| `0018-Report-synchronous-IOP-file-completion-…` | Report successful completion through ioman's ioctl status word; the original loader otherwise waits for already completed host I/O. |
| `0019-Add-optional-native-menu-loader-…` | Environment-gated loader/RPC diagnostics and bounded host-input injection for the native Windows test harness. |
| `0020-Prevent-VBlank-catch-up-bursts-…` | Bound overdue host field catch-up after slow guest frames; the later audio patch also advances guest clocks to the earliest due field. |
| `0021-Show-Verdite-mouse-capture-status-…` | The Verdite mouse glyph appears over the game picture when capture changes, with its MIT attribution. |
| `0022-Restore-native-audio-…` | Native SPU2 mixing and host PCM output, original IOP driver timing/refills, shared CD sectors, guarded USA SDRDRV RPC, MPEG callback/lifecycle and IPU accounting, EE scheduling, and generated continuation registration. Includes the RecompOne SPU notice. |
| `0023-Honor-disabled-GS-depth-testing-…` | Honour `TEST.ZTE=0`: bypass its stored depth comparison and suppress depth writes. The original opening-movie sprite now draws decoded texture pixels; inherited depth state cannot overwrite its texture. |
| `0024-Preserve-sprite-coordinate-pairs-…` | Preserve matched XY/UV endpoints when reversing sprite axes, fractional XYOFFSET/UV, ceil-exclusive coverage, integer GS sampling and flat second-vertex Q. Corrects the original 64-pixel gameplay feedback strips without changing the texture-cache policy. |
| `0025-Bound-GS-diagnostics-…` | Opt-in traces respect the selected tick interval for every event; privileged state includes CRT2, and primitive coordinates retain fractional XYOFFSET. Does not change rendering. |
| `0026-Decode-interlaced-source-height-…` | Decode interlaced FRAME-mode source rows before the host-size cap. Gameplay's encoded 896 display lines retain all 448 source rows, rather than cropping to 256 and falsely doubling them. |
| `0027-Add-Vulkan-GS-rasterization-…` | Optional paraLLEl-GS Vulkan compute rendering, transfers and logical scanout. Hardware Vulkan is preferred when compiled in; an explicit CPU reference remains available. Includes serialized cross-thread device access and GPU image readback for the existing host window. |
| `0028-Bob-current-Vulkan-fields-…` | Expand the current 224-row field to 448 rows on GPU without weaving older presentations into title/menu text. Preserve raw-field diagnostics and explicitly transition the skipped-deinterlace image for transfer. |
| `0029-Correct-EE-SQRT-and-RSQRT-source-operands` | SQRT.S reads Ft on the EE; RSQRT.S computes Fs / sqrt(abs(Ft)). Corrects ground-edge length calculations and handles source/destination aliases, signed zero and live/sticky exception flags. Rebuild tools and regenerate the game. |
| `0030-Add-bounded-opt-in-disc-and-IOMAN-read-diagnostics` | `PS2X_IO_TRACE` reports actual CD/IOMAN reads and completion, capped by `PS2X_IO_TRACE_LIMIT`; normal I/O behaviour is unchanged. |
| `0031-Trace-guarded-KFIV-world-and-collision-bank-state-on` | `PS2X_WORLD_TRACE` observes the USA game's player position and collision-bank readiness through the existing metadata/opcode-guarded input hook. Guest memory is read only. |
| `0032-Walk-complete-finite-DMA-chains-without-a-tag-count-` | Complete finite VIF/GIF source chains beyond 4096 tags, preserving tail FINISH packets. A constant-space traversal-state cycle detector protects against invalid loops and includes the CALL return stack. |
| `0033-Service-MPEG-output-callbacks-when-audio-backpressur` | Service the registered SDK UPDATE callback when a rejected audio packet blocks further video and the decoded queue is empty. Retain the packet for retry; rate-limit additional service by VSync and preserve cancellation guards. |
| `0034-Truncate-and-saturate-EE-CVT.W.S-independently-of-ho` | EE CVT.W.S truncates toward zero and saturates by sign regardless of host rounding mode. Corrects shared angle range reduction used by skeletal matrices; no character-specific pose override. |

The recompiler hunks in patches 0022 and 0029 must be applied **before generating
the game**. `00-build-tools.sh` applies the tool portions through the shared
patch helper before building `ps2_recomp`; `03-build-runner.sh` applies the
complete series to the runner checkout. Windows setup uses the same series.
Regenerate `output/` when adopting these patches; relinking old generated
function tables alone does not add the missing continuation entries.

The 34-patch series retains patches 0001–0014 unchanged. Twenty additions,
0015–0034, were exported from source commits on top of that series. The
native Windows validation and remaining movie/rendering limitations are
recorded in [`docs/windows-validation.md`](../docs/windows-validation.md).
The generator fix is proposed in
[PS2Recomp #279](https://github.com/ran-j/PS2Recomp/pull/279), and the
disabled-depth fix separately in
[PS2Recomp #280](https://github.com/ran-j/PS2Recomp/pull/280).
The sprite-coordinate fix is proposed separately in
[PS2Recomp #281](https://github.com/ran-j/PS2Recomp/pull/281).

Patch 0027 does not vendor the Vulkan dependency. Prepare its pinned source
and separate LGPL small-transfer patch with
`tests/gpu-gs-tests/Prepare-ParallelGS.ps1`; see [Vulkan rendering](../docs/vulkan.md).

PS2Recomp is GPL-3.0, so these patches are derivative works under the same
license, as are the maintainer tools built from its sources
(`scripts/maintainer/gsreplay`, `scripts/maintainer/dev/vu1-verify.patch`).
The rest of this repository is MIT (see `../LICENSE`).

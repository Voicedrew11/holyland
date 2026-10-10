# Vulkan GS rendering

Patch 0027 adds a hardware Vulkan compute GS backend to the native
recompiled runner. It uses the standalone paraLLEl-GS library, pinned to
`cc6184af7e0c03da603045ca371ffa5dae9b0655`, with Granite, volk and
Vulkan-Headers. The native game executable, original runtime and controls
remain the launch path.

## Build and select

Follow the [Windows build guide](windows.md). Prepare the dependency with
`tests/gpu-gs-tests/Prepare-ParallelGS.ps1`, then configure the runtime with
`PS2X_ENABLE_VULKAN_GS=ON` and `PS2X_PARALLEL_GS_SOURCE_DIR` pointing to that
prepared source. Vulkan GS is an optional build feature and defaults off
for existing CPU-only builds. The library ships generated SPIR-V, so a
Vulkan SDK or shader compiler is unnecessary. Execution needs a compatible
hardware Vulkan driver; software Vulkan devices are rejected.

`PS2X_GS_BACKEND` accepts `auto`, `cpu` or `vulkan`. An unset variable acts
as `auto`: Vulkan-enabled builds prefer hardware Vulkan, and log a CPU
renderer fallback if initialization fails. `cpu` keeps the software
reference available. Explicit `vulkan` propagates initialization failures;
it cannot silently become CPU rasterization. GPU failures during rendering
also propagate. `PS2X_GS_THREADS` affects the CPU backend only.

The initialization log identifies the Vulkan GPU and device IDs.
`PS2X_STATS=1` also reports cumulative Vulkan draws, GPU scanouts and CPU
display conversions every 300 presentations. Recording wraps the selected
backend, preserving the existing private GS replay workflow.

## What runs on the GPU

Decoded primitive batches become GS register writes, preserving paired
sprite endpoints, fractional coordinates and full 32-bit depth. Vulkan
handles rasterization, texture/CLUT sampling, mipmapping, fog, blend and
depth/alpha tests, framebuffer feedback, uploads and local transfers.
The adapter preserves automatically generated mip addresses when the
current frontend has no explicit MIPTBP1 value. All GS/device calls share
one mutex and register their Granite thread index, allowing EE/VU and host
presentation callers to use one serialized command-pool slot.

Scanout sampling, circuit merging and field deinterlacing also run
on Vulkan. The current presentation request has logical display geometry
but lacks SMODE1 clock state. The bridge therefore normalizes this logical
viewport rather than claiming analog PS2 timing equivalence. It supports
the tested 640 × 448 gameplay viewport and 224-row movie fields. Unsupported
viewports use an explicitly logged CPU display conversion of GPU VRAM;
their primitive rendering remains Vulkan. The retail verification observed
one oversized startup scanout using this conversion, before the title.

Patch 0028 presents packed 224-row fields with a nearest-neighbor GPU bob
to 448 rows. The previous adaptive weave reused earlier presentation images,
which visibly displaced alternating rows in the title and inventory text.
Runtime presentations can repeat field parity or skip ticks, so that history
does not guarantee an opposite field. Bob keeps every output row from the
current field, preserving its original pixel detail without temporal ghosts.
Normal 448-row progressive scanout and GS rasterization are unchanged.
`PS2X_NO_WEAVE=1` retains the raw 224-row field for diagnostics. The bridge
explicitly transitions skipped-deinterlace images from read-only to transfer
layout before the GPU blit or raw readback.

The final image is fenced and read back as RGBA for the existing Raylib
OpenGL window. This retains a CPU row-copy/alpha-normalization step and
host texture upload. It is not an end-to-end Vulkan swapchain or zero-copy
presentation implementation. GPU/host interop remains a possible follow-up.

## Native validation and performance limits

Native Windows 11 x64, MSVC 19.44, `/O2 /Ob1 /fp:strict`, the i9-13900KF and
RTX 4090 were tested. A strict Vulkan run entered title/New Game/brightness,
displayed and skipped the opening, reached gameplay, moved/turned, opened
and closed inventory, and paused/resumed through scripted keyboard input.
It reached tick 6000 and exited normally in 187.781 seconds. The matching
CPU run took 196.591 seconds: about 4.5% less wall time for this pair.
Later gameplay remained around 10–12 sampled display flips/s. The host
window refresh rate is not the game's frame rate.

The exact installed build also passed an unset/default backend run to tick
4500 in 119.108 seconds, with 46 captures and normal exit. Its log selected
the RTX 4090 and sampled 8,747,951 Vulkan draws, 4,199 GPU scanouts and one
startup CPU display conversion. Menu and pause captures returned to
gameplay. A separate explicit `cpu` launch reached tick 600 normally in
10.962 seconds without constructing Vulkan. Existing shortcut target,
arguments, working directory and the 28 shared runtime DLLs were retained.

After patch 0028, a native keyboard-input run reached tick 4500 in 118.064
seconds with normal exit and 46 captures. Title and inventory text no longer
show the earlier alternating-line overlay; opening pictures, gameplay,
inventory back, pause and resume remain available. Eight scanout cases plus
the raster fixture pass, as do two optional old-source controls (11/11
CTests). The old field-history path fails 1,290,243 generated pixel checks,
while the new history/raw cases pass all 4,300,880 checks. A fresh 28-patch
application and idempotent repeat reproduce all 62 managed source files.

A separate cached gameplay-command replay measured CPU1 at 22.703 seconds,
CPU8 at 8.780 seconds and Vulkan at 1.247 seconds for the same 1,180,521
submissions, including final synchronization/readback. The 718 MB command
window was loaded before timing, so asynchronous work could not hide in
disk I/O. This is about 7× faster than CPU8 for the isolated replay; it is
not a whole-game speedup. CPU1/CPU8 sample images were identical. The GPU
final image retained 640 × 448 geometry and HUD with RGB MAE 1.230/255 and
1.064% of pixels differing by more than eight levels from the CPU reference.

This single pair is not a weak-CPU benchmark or full-speed guarantee.
Vulkan removes CPU rasterization, but IOP/VU interpretation and other
runtime CPU work remain. Shader first use, the synchronous image-readback
bridge and driver support also affect performance. No native Linux GPU
execution, other GPU models, later areas or full playthrough are verified.
Broader hardware accuracy and audio timing remain open.

The [source-only GPU package](../tests/gpu-gs-tests/README.md) provides
controlled raster/transfer, cross-thread and scanout tests, plus a private
recording replay with separate correctness and performance modes. A new
renderer can differ from the CPU reference's interpolation, mipmapping
and scan-mask behavior; it is not presented as a bit-identical optimization
of that reference. Controlled expected pixels and measured retail image
differences are used instead of assuming all reference pixels are correct.

See the package's [dependency guide](../tests/gpu-gs-tests/DEPENDENCIES.md)
and [license notices](../tests/gpu-gs-tests/THIRD-PARTY-NOTICES.md).
The separately supplied LGPL small-transfer patch corrects rounded word
counts and padded FIFO storage in the pinned dependency. Disc assets,
generated game code, recordings, screenshots and binaries stay private.

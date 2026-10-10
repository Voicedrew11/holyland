# Native Vulkan GS checks

This source-only package tests the actual PS2Recomp `GSRasterBackend` CPU and
Vulkan implementations. It contains synthetic pixels and geometry, with no
retail executable, disc data, textures, or captured command stream.

The tests and replay utility are licensed under GPL-3.0-only, matching the
runtime they compile against. ParallelGS is a separate LGPL-3.0-or-later
dependency, and Granite and its submodules retain their own licenses. Obtain
their source and notices with the pinned runtime dependency instructions.

See [DEPENDENCIES.md](DEPENDENCIES.md) for the exact ParallelGS/Granite pins,
minimal submodules and preparation script. Apply the included
`parallel-gs-small-transfers.patch` to that pinned source: it fixes rounding of
short transfer rectangles and padded FIFO readback tails. The source patch
retains ParallelGS's LGPL license. It is needed for the short-transfer tests.
See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the retained notices.

Supply both source paths explicitly; no machine-specific workspace is assumed:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./Prepare-ParallelGS.ps1 `
  -SourceDirectory C:/src/parallel-gs
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DPS2RECOMP_DIR=C:/src/PS2Recomp -DPARALLEL_GS_DIR=C:/src/parallel-gs
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The default Windows runtime is `/MD`, matching the native runner. Override
`CMAKE_MSVC_RUNTIME_LIBRARY` only when the entire linked runtime uses the same
choice. The Vulkan test needs a compatible installed GPU driver; building it
does not install drivers. `GPU_GS_CPU_CONTROL_ONLY=ON` instead builds a synthetic
two-CPU fixture control, which does not prove Vulkan support or GPU execution.

`gs_gpu_replay` consumes an opt-in private `PS2X_GS_RECORD` stream produced by the
same runtime ABI. GSRecord serializes native structures without a version header,
so a recording must match the checkout, pointer size and compiler ABI used here.
It validates truncation and opcodes and reports the relevant structure sizes.
Never distribute a retail recording or its images with this source package.

```powershell
build/Release/gs_gpu_replay.exe C:/private/gs-record.bin --backend both `
  --from-tick 3300 --until-tick 3600 --dump-dir C:/private/frames
$env:PS2X_GS_THREADS = '1' # Repeat the CPU command with '8' for eight workers.
$env:PS2X_STATS = '0'
build/Release/gs_gpu_replay.exe C:/private/gs-record.bin --backend cpu `
  --from-tick 3300 --until-tick 3600 --cache-window-mib 1024
$env:PS2X_GS_THREADS = '8' # Only affects the Vulkan backend's CPU helper.
build/Release/gs_gpu_replay.exe C:/private/gs-record.bin --backend vulkan `
  --from-tick 3300 --until-tick 3600 --cache-window-mib 1024
```

All commands before the requested tick are executed to preserve CLUTs, VRAM and
transfer state. With `--cache-window-mib`, the utility first preloads the bounded
command window into RAM, then completes the warmup with a synchronized VRAM
readback before starting the clock. Cached replay wall time includes command
parsing, API calls, per-submit rounding, presentations and a final synchronized
VRAM readback. It excludes file reads and image writes, so neither asynchronous
renderer receives unmeasured execution time during disk input. The cache limit
is at most 2048 MiB and incomplete windows fail. Initialization is separate;
intermediate PPM output is suppressed during a cached window.

Streaming replay remains the default and reports the sum of backend call times.
That sum excludes input/output but permits asynchronous progress between calls,
so it must not be interpreted as complete raster wall time. For performance,
compare isolated CPU and Vulkan cached runs using the same recording, tick range
and explicit CPU worker count after shader warmup. A combined streaming run is
useful for image checks, but concurrent CPU/GPU activity should not support a
speed claim. Replay timings describe this replay harness, not native game FPS.

The replay reports RGB exact-pixel percentage, differences greater than eight,
MAE, RMSE, dimensions and transfer/readback differences. Retail interpolation,
mipmapping, scan masking and feedback may differ from the current CPU renderer;
these metrics identify candidates for visual review and do not by themselves
prove a regression. `--require-exact` is available for known constant-value
recordings. Synthetic tests supply controlled expected values rather than
assuming that every GPU triangle is byte-identical to the CPU implementation.

Native Windows 11, MSVC 19.44 x64 Release `/MD`, Intel i9-13900KF and NVIDIA
RTX 4090 (Vulkan 1.4.351) validation passed all seven CTest cases. The raster
fixture passed 70 cases and 857 checks with zero failures, including creation on
one thread, uploads/draws from other threads, main-thread presentation and
concurrent serialized callers. CTest also rejects the thread-manager error
observed during development. The two-CPU expectation control passed 67 cases
and 853 checks; its three skipped GPU-only cases cover
automatic mip level one and scan masks that the current CPU rasterizer does
not implement. Gouraud gradients use separate CPU half-pixel and GS integer
sample oracles, with attribution in the source.

The separate `gs_gpu_scanout` target exercises CRT1, CRT2, dual-circuit merging,
224-row movie fields, magnification/narrow viewports and rejected oversized
viewports. These tests use generated gradients to check every output row and
column, including the bottom HUD rows. They call the actual GPU scanout helper
and require a hardware Vulkan device; a software Vulkan device is rejected.
All six cases passed 3,584,052 generated-pixel/state checks with zero failures.

The final private gameplay replay used a 717,976,328-byte cached window after
tick 3300 through the first presentation at or beyond tick 3600 (tick 3601).
Each isolated run executed the same 1,180,521 submissions and 290 timed
presentations, followed by synchronized VRAM readback. The 1024 MiB cache limit
was sufficient. Statistics logging was disabled; the fixture uses `/fp:strict`,
and the separate ParallelGS/Granite libraries use their upstream `/fp:precise`.

| Renderer | Explicit CPU raster workers | Cached replay wall time |
| --- | ---: | ---: |
| CPU | 1 | 22.703334 s |
| CPU | 8 | 8.779501 s |
| Vulkan | GPU rasterization; helper configured for 8 | 1.247156 s |

The CPU outputs with one and eight workers were byte-identical at both sampled
640×448 presentations. Relative to the eight-worker CPU output, Vulkan's final
frame had RGB mean absolute difference 1.230/255, RMSE 2.088/255 and 1.064% of
pixels differing by more than eight in any RGB channel. The scene geometry and
full bottom HUD remained aligned in visual review. These are two sampled
frames, not a claim of bitwise equality throughout gameplay. One unsupported
oversized early-boot image used CPU display conversion before the measured
window; rasterization remained Vulkan.

The native game test through 6000 ticks took 196.591 s using the default CPU
renderer and 187.781 s using Vulkan, a 4.5% improvement in elapsed time. It did
not show the replay's several-fold improvement in overall gameplay speed.
The native runner retains CPU guest execution and synchronous image readback
and host presentation. These hardware-specific results do not establish a
speedup on a weaker CPU, another GPU, or a full playthrough. No Linux execution
or other-platform GPU validation is claimed. Private recordings, screenshots
and binaries are intentionally excluded from this source package.

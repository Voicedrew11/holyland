# Source-only regressions

These packages exercise the actual patched PS2Recomp source and libraries
using synthetic inputs. They contain no game executable, disc assets,
generated retail C++, compiled binaries or local result logs. There are
21 standalone fixture packages plus the separate audio-loopback diagnostic.
Native Windows MSVC 19.44 x64 validation passed the original 16 packages
and 26 CTest entries after relocation here. The added GS depth regression
passed 143,397 checks and two CTest entries with a prior-behaviour control.
The new alignment fixture passed 853,552 checks with one and eight workers;
the bounded-trace fixture passed 59 assertions across nine fixed cases, with
six additional controlled old-behaviour cases. The new source-height fixture
passed 6,129,642 checks across nine fixed cases and six additional controls.
All three new packages were rebuilt after relocation. The original packages
were not rerun solely to add these fixtures.
They are not a full gameplay or PS2 hardware-equivalence
test; retail evidence is recorded in [Windows validation](../docs/windows-validation.md).

The optional [GPU GS package](gpu-gs-tests/README.md) adds hardware Vulkan
raster/transfer, cross-thread and scanout fixtures, pinned dependency setup,
and a private recording replay. It contains synthetic inputs only and
requires a compatible hardware GPU for its Vulkan cases. See
[Vulkan rendering](../docs/vulkan.md) for native integration evidence and
the limited whole-game speed improvement.

All files in `tests/` are provided under GPL-3.0, consistent with the
PS2Recomp code they exercise or derive from. See [LICENSE](LICENSE).
In particular, `vblank-tests/control/EeScheduler.cpp` is the original
PS2Recomp GPL source from pinned commit
`c5a9d02573410a2085a4b4b831b0b68ba3515440`, retained as a regression control.
The MIT SPU implementation's existing notice remains in the production
source. Repository scripts outside `tests/` retain the repository's MIT license.

## Configure and run

First build the patched tools and native runtime as described in the
[Windows guide](../docs/windows.md). Rebuild production libraries after
source or header changes before running fixtures that link those libraries.
The packages do not build the retail runner or obtain disc files.

Each package accepts `PS2RECOMP_DIR`, pointing to the patched source checkout.
Packages linking runtime libraries also need `RUNTIME_BUILD_DIR`; the
generator fixture needs `TOOLS_BUILD_DIR`. There are no workspace defaults.
`RAYLIB_INCLUDE_DIR` and `FFMPEG_DIR` can override the corresponding fetched
dependencies. `RUNTIME_CONFIG` defaults to `RelWithDebInfo` and
`TOOLS_CONFIG` to `Release`. Build runtime-linked fixtures in the same
configuration as the production libraries; some packages use the fixture's
CMake configuration directly when linking. MSVC fixtures use `/fp:strict`.

For one package, from the repository in PowerShell:

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$checkout = 'C:\KFIV-dev\PS2Recomp'
$runtime = 'C:\KFIV-dev\runtime-build'
$tools = 'C:\KFIV-dev\tools-build'
$build = 'C:\KFIV-dev\test-builds\mpeg-tests'
& $cmake -S tests/mpeg-tests -B $build -G 'Visual Studio 17 2022' -A x64 `
  "-DPS2RECOMP_DIR=$checkout" "-DRUNTIME_BUILD_DIR=$runtime" "-DTOOLS_BUILD_DIR=$tools"
& $cmake --build $build --config RelWithDebInfo --parallel 2
& $ctest --test-dir $build -C RelWithDebInfo --output-on-failure
```

Repeat for the packages below. Use `Release` for
`recompiler-resume-tests`, matching its tools libraries; use
`RelWithDebInfo` for the other packages in the verified setup. Missing path
settings fail at configuration. FFmpeg-dependent test executables stage
their matching DLLs automatically. Keep test build directories external.

## Scope and substituted boundaries

Counts below include controlled negative cases where applicable. Controls
restore the specific old defect in build scratch, preserving production
sources; expected failure behaviour is itself asserted.

| Package | Checks | Actual implementation and fixture boundary |
|---|---:|---|
| `input-tests` | 453 | Real pad mapper with simulated Raylib keyboard/mouse/gamepad devices; bindings, capture, menu transitions, glyph and snapshots. Physical GLFW delivery needs integration testing. |
| `menu-tests` | 1,735 | Real KFIV binding, override registry and IOP allocator; controlled EE/SDK boundary supplies Free's SIF contract. Metadata/opcode guards and 300 allocation cycles; resource loading is not bypassed. |
| `ioman-tests` | 79 | Actual IOMAN and IOP memory, using a real temporary file; read/seek/short read and async completion status 0, invalid descriptors and RAM bounds. |
| `vblank-tests` | 105 | Real runtime scheduler and WaitVSyncTick; a 200 ms native call exercises overdue deadlines. Pinned upstream scheduler is the burst-reproduction control. |
| `audio-tests` | 607 | Actual SPU2/MMIO/DMA and IOP RAM with synthetic ADPCM/PCM; voices, ADSR, pitch, mixing, reverb, AutoDMA reserve, STOP and restart. Hardware approximations remain. |
| `audio-host-tests` | 125 | Actual host audio backend/VAG decoder with a simulated Raylib audio device; prebuffer, concurrency, bounded queues, underrun silence and WAV shutdown. |
| `audio-clock-tests` | 97 | Actual IOP subsystem/interpreter with executable synthetic MIPS IRX timer/RPC fixtures; deterministic/monotonic clocks, timer callbacks and interrupt suspend/resume. |
| `audio-dma-tests` | 52 | Actual IOP interpreter, intrman and SPU2; synthetic guest IRQ restarts AutoDMA. Guard-disabled control reproduces post-STOP callbacks and unrelated RAM corruption. |
| `remote-audio-tests` | 3,269 | Actual metadata-guarded KFIV bridge/registry against a controlled SIF boundary; physical-server requirement, blocking transport, packet shape, stack and scratch lifetime. Real audio requires integration. |
| `cd-audio-tests` | 31 | Actual CDVD imports/kernel/SPU with synthetic host sector mapping; file-backed versus physical-sector selection, offsets and failure propagation. |
| `ipu-input-tests` | 115 | Actual PS2Memory with synthetic RAM/DMA tags; unrelated GS/VIF link stubs are inert. Accepted-byte credits retire the real channel-4 chains; not a full hardware IPU parser. |
| `mpeg-tests` | 412 | Actual MPEG source, real EE scheduler/runtime and FFmpeg. Synthetic PES/rings, decoded-picture hook for lifecycle cases, and genuine three-frame decode for video/cancellation. Windows protected pages check bounded retry reads. |
| `ee-priority-tests` | 63 | Actual kernel thread syscalls and scheduler, synthetic guest bootstrap and empty function table; original priority-zero rejection is the control. |
| `ee-callback-tests` | 244 | Actual scheduler/guest heap and synthetic guest callbacks; stale-entry cancellation, started continuations and tuple lifetime. Predicate-disabled scheduler is the control. |
| `ee-clock-tests` | 150 | Actual scheduler/timers and synthetic busy-poll guest threads; host-deadline field credit while ready threads run. Credit-disabled control reproduces starvation. |
| `recompiler-resume-tests` | 341 | Real ELF/config/decoder/emitter pipeline with synthetic MIPS ELF; synthesized entry aliases, explicit handler precedence and deterministic ownership. Precise old generator behaviour is restored in the control. |
| `gs-depth-tests` | 143,397 | Actual CPU GS backend and local memory with synthetic colors/texture; disabled and enabled depth tests, depth-write masking, movie-style DECAL sprite and CRT2 scanout. A scratch control removes only the ZTE fix. No decoder or retail assets. |
| `gs-alignment-tests` | 853,552 per worker count | Actual CPU GS backend and local memory; scalar fixed-UV oracle, reversed axes, fractional offsets, bounds/scissor, flat Q and serial 64-pixel framebuffer feedback through the real texture cache. One/eight workers; optional prior-sprite control. No retail assets or presentation changes. |
| `gs-trace-tests` | 59 fixed | Actual opt-in GS trace implementation with synthetic register, transfer, draw and privileged CRT state; inclusive tick bounds, CRT2 fields and fractional XYOFFSET diagnostics. Nine fixed cases and six controlled prior-behaviour cases. |
| `gs-scanout-height-tests` | 6,129,642 fixed | Real backend framebuffer upload and public presentation through its VRAM snapshot. Independently encoded gradients/HUD marker check full source rows, CRT1/CRT2/dual, magnification, odd rounding, clamp order, progressive/FIELD and initial small FRAME bob. Nine fixed cases, six prior-height controls; no cross-frame history or window scaling claim. |

MPEG's 412 checks comprise 54 SDK lifecycle, 75 demux, 84 video, 93
cancellation and 106 snapshot checks. They cover committed B9 completion
despite unread sector padding, no callbacks after B9, original SDK
`work + 4` reference count, final picture timing and reset/recreate lifetime.
B7 alone must remain incomplete until accepted B9 or physical producer EOF.

The synthetic 1,065-byte clip is encoded in
`mpeg-tests/fixtures/video_three_frames.h` as source text. It was generated
from FFmpeg's synthetic `testsrc2`, with no game input:

```sh
ffmpeg -f lavfi -i testsrc2=size=16x16:rate=30 -frames:v 3 -c:v mpeg2video -g 1 -bf 0 -f mpeg2video video-three-frames.m2v
```

The generator tests cover separate serial/parallel output and combined
serial output. Combined parallel output encountered the pre-existing
"combined output completion queue is missing index" issue in both fixed
and control generators; it is outside this fix. KFIV uses separate files.

The GS depth package's two CTest entries include an expected-failure
control, which reproduces 143,369 failed checks with the previous ZTE
behaviour. Focused upstream GS checks passed 51/51. At the depth-fix stage,
the full legacy GS suite passed 41/72 in both fixed and prior controls, with the same 31
failures; the complete legacy suite is not green.

The alignment package's two default CTest entries run with one and eight
GS workers. Supplying `GS_ALIGNMENT_PRIOR_SOURCE` adds an expected-failure
control; the verified control replaces only `DrawSprite` in otherwise
identical scratch source. It reproduces 46,839 failed checks, 7,812 changed
feedback pixels and 217 extra strip/page boundary steps. The fixed identity
feedback pass has zero changed pixels and zero extra steps. This does not
assert a new cache policy for all possible feedback effects.

The trace package passed all 15 CTest entries: nine fixed cases total 59
assertions, and six expected-failure controls total 43 checks with 14
failures under the old behaviour. Diagnostics are off by default and these
tests do not assess gameplay rendering.

The scanout-height package has nine default CTest cases. Supplying
`GS_SCANOUT_PRIOR_SOURCE` adds six expected-failure controls, which reproduce
4,072,731 failures in 4,081,608 checks. All 15 CTest entries passed. The
default-environment CRT2 case also passed 1,146,904 checks, with normal weave
and worker defaults. The fixture checks all pixels and alpha, including a
synthetic marker on source rows 416–447; it covers the source-height ordering
without asserting full cross-frame weave accuracy.

## Script safety tests and audio diagnostic

`python tests/test_apply_patches.py` runs seven portable stdlib/Git tests
using private temporary repositories. They cover tools twice, full after
tools, full twice, tools after full, unchanged real index, unrelated edits,
wrong HEAD, changed patches/phase record/managed files, and incomplete-phase
failure. The series now contains 27 patches. Before adding the GS depth
fix, the 22-patch series was also tested on native Windows with
`core.autocrlf=true`: tools twice, full after tools and full twice reproduced
the verified complete source tree after Git normalization.

The complete 26-patch series was also replayed with tools application twice,
full after tools, and full application twice. Staging new files and applying
Git line-ending normalization reproduced the tested source-export tree
`6c3edbccc9e71f1477863bd707e5758658e9b28b` exactly, with all 57 managed
source files verified after normalization.

The 27-patch Vulkan series was applied from a fresh pinned checkout and
repeated idempotently; all 62 managed normalized files match the source
export. GPU package relocation builds against this fresh patched source,
rather than requiring generated game code or a private runtime library.

`audio-loopback` is an optional Windows-only diagnostic, built separately
with CMake and no PS2Recomp path settings. It requires Windows build 20348+
and uses WASAPI process loopback to capture only a supplied process tree:

```powershell
& $cmake -S tests/audio-loopback -B C:\KFIV-dev\test-builds\audio-loopback -G 'Visual Studio 17 2022' -A x64
& $cmake --build C:\KFIV-dev\test-builds\audio-loopback --config RelWithDebInfo
& C:\KFIV-dev\test-builds\audio-loopback\RelWithDebInfo\kfiv_process_audio_capture.exe 12345 C:\KFIV-dev\loopback.wav 600
```

Replace `12345` with the running game's PID. The diagnostic reads no
microphone or other applications' render streams. Its text summary reports
frames, nonzero samples, peak, packets and discontinuities. The analyzer at
`scripts/windows/Analyze-KFIVAudio.py` reads signed 16-bit stereo WAVs using
only Python's standard library.

# Source-only regressions

These packages exercise the actual patched PS2Recomp source and libraries
using synthetic inputs. They contain no game executable, disc assets,
generated retail C++, compiled binaries or local result logs. Native Windows
MSVC 19.44 x64 validation passed all 16 packages and 26 CTest entries after
relocation here. They are not a full gameplay or PS2 hardware-equivalence
test; retail evidence is recorded in [Windows validation](../docs/windows-validation.md).

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

## Script safety tests and audio diagnostic

`python tests/test_apply_patches.py` runs seven portable stdlib/Git tests
using private temporary repositories. They cover tools twice, full after
tools, full twice, tools after full, unchanged real index, unrelated edits,
wrong HEAD, changed patches/phase record/managed files, and incomplete-phase
failure. The real 22-patch series was also tested on native Windows with
`core.autocrlf=true`: tools twice, full after tools and full twice reproduced
the verified complete source tree after Git normalization.

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

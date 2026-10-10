# Bounded field-pacing fixture

This source-only package tests the actual `VBlankFieldPacer` header from an
explicit patched PS2Recomp checkout using synthetic steady-clock timestamps.
It does not link the scheduler, read retail input, run the game, or create a
host window. The companion [`vblank-tests`](../vblank-tests/README.md) exercises
the actual linked scheduler and waiter dispatch on native Windows.

For emitted Start `E`, blank duration `D`, field period `T`, successor parity
`q`, and one steady-clock duration tick `epsilon`, the helper chooses:

```
q unseen: next = E + T
q seen:   next = max(E + D + epsilon, lastStart[q] + 2*T)
```

Each next Start strictly follows the preceding End. By induction, each
published parity remains at least two selected field periods
apart. Late work is not rounded up to a global field grid. Missed host time
does not create additional guest fields; guest counts and cycle offsets remain
separate from these host deadlines.

## Build and run

Use CMake 3.21 or newer and a C++20 compiler. No runtime library, FFmpeg,
graphics hardware, or disc is required. Keep build output outside this source
package and repository. From the repository in native Windows PowerShell:

```powershell
$checkout = 'C:\KFIV-dev\PS2Recomp'
$build = 'C:\KFIV-dev\test-builds\field-pacing-tests'
cmake -S tests/field-pacing-tests -B $build -G 'Visual Studio 17 2022' -A x64 `
  "-DPS2RECOMP_DIR=$checkout"
cmake --build $build --config Release --parallel 2
ctest --test-dir $build -C Release --output-on-failure
```

Native MSVC 19.44 x64 Release validation passed all four CTests: 19,359,261
legacy-fallback checks, 16,811,928 interlaced NTSC checks, and both mathematical
controls. The package also supports a standard CMake C++20
toolchain; Linux compiler or runtime validation is not implied.

## What is checked

The main test includes 200,000 deterministic randomized work durations, each
with simulated 60 Hz, 300 Hz, and uncapped presentation reads. It checks:

- A late opposite field at 25 ms reaching the gameplay parity at 33.334 ms.
- A late gameplay field retaining its full next same-parity interval.
- End ordering, unseen parity, lifecycle reset, repeated overruns and recovery.
- Every emitted same-parity pair staying at least 33.334 ms apart.
- Zero-work nominal 60 Hz field spacing and presentation reads changing no state.
- One guest field per emission and unchanged period/End cycle-offset formulas.
- Repeated 34, 35 and 40 ms work avoiding an extra 16.667 ms grid slot.

Those numeric cases use the retained 16,667 us fallback. The separate NTSC
test uses 16,683,333 ns, verifies the exact 4,920,116-cycle scheduled offset,
and repeats 200,000 randomized fields under the same presentation rates.
The companion linked mode fixture selects that period through the actual GS
API only for interlaced BIOS mode 0x02. Other modes retain the existing
fallback; this package makes no claim of complete PAL or progressive timing.

The additive-wait control reproduces the former 25 ms case's 41.667 ms
successor instead of 33.334 ms. The grid control reproduces 50.001 ms after
35 ms work, while the current helper schedules 35.500 ms plus one clock tick.
These are explicit mathematical controls, not alternative compiled runtimes.

Individual opposite fields can be close during recovery. Independent parity
bounds alone do not prove that a guest observes both: a previous continuous
helper passed these mathematical checks but lost fields in private retail
observation because another Start could precede the awakened continuation.
The scheduler handoff and joint host/cycle eligibility changes are tested
separately by the linked fixture. Long blocked guest calls or priority
starvation can still miss fields legitimately.

This fake clock proves bounds without depending on Windows sleep precision.
It does not prove host input delivery, arbitrary guest observability, original
hardware equivalence, or achieved whole-game speed. Those require the linked
fixture and isolated private game measurements.

Files in this package are GPL-3.0; see [`../LICENSE`](../LICENSE).

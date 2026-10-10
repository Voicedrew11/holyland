# EE wall clock and chronological timer fixtures

This source-only native Windows package links actual PS2Recomp memory, EE
scheduler, IOP, VSync syscalls and hardware timers. It uses authored functions
and real RAM, without a disc, generated retail code or recording.

The hardware clock has a reset-anchored steady-clock floor. Dispatch reconciles
elapsed native work chronologically to real timer and scheduled-event
boundaries, letting enabled timer IRQ handlers execute before more credit.
Already accounted guest cycles are not credited again. Field publication is
independent: scheduled Start/End offsets remain exact, while actual elapsed
hardware cycles can exceed a delayed field's scheduled cycle.

Timer MMIO synchronizes time spent inside the currently executing native
function before accessing the real timer. An indivisible native function
cannot be retroactively interrupted. Its elapsed periods legitimately coalesce
while EQUF remains latched. In a long ISR, synchronization before MODE's W1C
acknowledgement preserves that hardware behavior; it does not replay historical
IRQs after clearing the latch. Prompt historical ISR service instead retains
chronological opportunities for the remaining prior dispatch debt.

Three groups check these contracts:

- Two Ready native polling threads with a VSync waiter, including a 200 ms
  busy call; consistent delivered publications, exact scheduled offsets,
  ordered End delivery, timer IRQ and BUS/256 accounting.
- Nine wall-clock modes: 23 ms native work before another timer helper,
  fragmented IRQ service, a 45 ms delayed acknowledgement, masked/missing
  handlers, scheduler reset, idle physical wait, instruction cycles already
  ahead of wall time, and repeated long-call MMIO with genuine re-arming.
- Ten chronological Timer0 modes use COMP=60000 and MODE=0x1C1. Prompt ISR
  rollovers reconstruct hardware ticks; masked, missing or blocked handlers
  preserve legitimate coalescing. Additional cases cover two timers, multiple
  handlers, an earlier alarm, and an unrelated callback that waits or polls
  while a timer interrupt is queued.

Two narrow controls are generated from the actual scheduler into build scratch.
One disables only the wall-floor function: busy polling must starve physical
events and native work must receive insufficient clock credit. The other
disables the timer boundary and service hold, while retaining the oscillator,
ordinary event ordering and hardware latch: software rollovers must be lost.
Exact source markers fail configuration if either removal needs review. No
reference runtime snapshot is committed and the checkout is never edited.

Rebuild all runtime units and consumers after header changes. PS2Memory's
optional timer callback changes its size and therefore PS2Runtime's layout;
mixing old GS/allocator objects with new headers is unsafe. A Vulkan runtime
also needs its matching already-built paraLLEl-GS/Granite link dependencies.

From a native Visual Studio x64 PowerShell environment:

```powershell
$checkout = 'C:\KFIV-dev\PS2Recomp'
$runtimeBuild = 'C:\KFIV-dev\runner-build'
$build = 'C:\KFIV-dev\test-builds\ee-clock-tests'
cmake -S tests/ee-clock-tests -B $build -G 'Visual Studio 17 2022' -A x64 `
  "-DPS2RECOMP_DIR=$checkout" "-DRUNTIME_BUILD_DIR=$runtimeBuild" `
  -DRUNTIME_CONFIG=RelWithDebInfo
cmake --build $build --config Release --parallel 2
ctest --test-dir $build -C Release --output-on-failure
```

Python 3 prepares controls; MSVC executes the tests with strict floating-point
settings. Fifteen-second per-test limits include actual runtime/GPU-backend
construction and teardown. This does not test renderer performance.

Native MSVC 19.44 x64 Release validation passed all 23 CTests against the fully
rebuilt final runtime, including ten wall-clock/undercredit cases and all
chronological positives and corruption controls. No test claims retail 30 Hz, universal guest
observability, exact PS2 hardware timing or Linux validation. See
[`vblank-tests`](../vblank-tests/README.md) for waiter lifecycles and
[`field-pacing-tests`](../field-pacing-tests/README.md) for pure rate bounds.
Tests are GPL-3.0; see [`../LICENSE`](../LICENSE).

The baseline scheduled-offset fixtures intentionally leave video mode unconfigured
and exercise the prior 16,667 us fallback. Confirmed interlaced NTSC selection
uses 16,683,333 ns and 4,920,116 cycles through the actual GS mode API; separate
mode fixtures cover that selection and preserve pending End/alarm order.
Ordinary SDK waits sample CSR FIELD on continuation resumption.

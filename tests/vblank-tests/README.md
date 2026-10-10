# Linked scheduler field-pacing fixture

This native Windows source-only package links the rebuilt PS2Recomp runtime
and exercises its real EE scheduler, `WaitVSyncTick`, sampled CSR/flag publication,
GS FIELD parity, and VBlankEnd guest IRQ handler. Authored native callbacks
provide synthetic guest workloads; no game inputs or assets are included.

## Build and run

Use an explicit patched checkout and its fully rebuilt runtime. Rebuild all
runtime units after changing scheduler headers: `PS2Runtime` allocates the
scheduler, so linking a new scheduler object against an allocator compiled
with the old class size is unsafe. The positive fixture links the existing
runtime directly. Each source-compiled control uses the same current header
and matching runtime allocation.

From the repository in a native Visual Studio x64 PowerShell environment:

```powershell
$checkout = 'C:\KFIV-dev\PS2Recomp'
$runtimeBuild = 'C:\KFIV-dev\runner-build'
$build = 'C:\KFIV-dev\test-builds\vblank-tests'
cmake -S tests/vblank-tests -B $build -G 'Visual Studio 17 2022' -A x64 `
  "-DPS2RECOMP_DIR=$checkout" "-DRUNTIME_BUILD_DIR=$runtimeBuild" `
  -DRUNTIME_CONFIG=RelWithDebInfo
cmake --build $build --config Release --parallel 2
ctest --test-dir $build -C Release --output-on-failure
```

A Vulkan-enabled runtime also requires its matching paraLLEl-GS and Granite
static dependencies, which CMake locates in the explicit runtime build. This
fixture does not rebuild those libraries or exercise GPU rendering. Its
remaining dependencies are the actual runtime, IOP, raylib, and FFmpeg
libraries and DLLs; they are not substituted.

## What is checked

The original three fixtures inject a 200 ms callback or 25/35 ms work across
opposite fields. They check sequential delivered ticks, publication before
resume, parity, exact guest cycle offsets, and End-before-next-Start ordering.
`SetVSyncFlag` publishes the full 64-bit sampled CSR and arms a one-shot flag;
it does not publish the scheduler's software field counter. Ordinary SDK waits
sample current CSR FIELD when their continuation actually resumes. Direct
scheduler waits retain the FIELD captured at wake, as the nested tests verify.
The captured upstream scheduler supplies a separate original-burst control.
Two no-op interface adapters exist only in that old control, which predates
timer MMIO synchronization and video-mode selection. Its source remains
pinned and its old software-tick publication expectation is isolated.

Sixteen handoff modes add coverage for separate wait/resume functions, an
8 ms callback with two fields of guest cycle debt, ordinary alarm delivery
during a hold, multiple awakened waiters, WaitingSuspended and Ready
suspension, deletion/self-delete, nested invocation waits, completion changing
PC to zero, missing/cancelled continuations, cooperative polling, priority
starvation escape, Stop, no waiters, and 35 ms work.

The handoff tracks the exact awakened (thread ID, invocation sequence) or
base continuation. Unrelated queued GS/IRQ callbacks, resume completion,
ordinary guest returns and fragmented call safe points retain that identity.
Its next blocking wait, suspension, deletion or invocation removal retires it.
A two-field expiry preserves priorities when a waiter cannot progress. Only
subsequent Starts are held: Ends, alarms and IRQ queues retain normal order.
Jointly host- and cycle-due eligibility lets runnable work register its next
wait before a future field.

Scheduled Start/End cycle offsets stay fixed. The actual hardware cycle ledger
also has an independent reset-anchored wall-time floor, so a delayed field's
emission cycle need not equal its scheduled cycle. Tests compare exact
scheduled offsets separately from monotonic actual elapsed cycles.

The continuous-only control compiles the actual current `EeScheduler.cpp`
with `PS2X_VBLANK_HANDOFF=0` and `PS2X_VBLANK_HOST_ELIGIBILITY=0`. It must
reproduce missed fields under the callback-debt workload. Both controls are
private build products and never modify production source.

Native MSVC 19.44 x64 Release validation passed all 22 CTests against the fully
rebuilt final runtime. The mode-transition test uses the actual `SetGsCrt` and
`sceGsResetGraph` paths and passed 200,782 checks across 100,000 transitions.
Its friend declaration is mechanically injected into a private build header;
it adds no data members or shipping access. It verifies pending Start cycle
and host retiming, preserved End/alarm ordering and retained parity history,
exact waiter identities, two-period expiry, fallback and repeated-mode no-op.
These are authored scheduler tests, not retail speed measurements or Linux
validation. Revalidation is required after any runtime or layout change.

Same-parity callback observations allow 1.334 ms of dispatch jitter around
the helper's exact 33.334 ms publication bound. The companion
[`field-pacing-tests`](../field-pacing-tests/README.md) proves that exact
bound, End ordering and presentation-read independence with a fake clock.
[`ee-clock-tests`](../ee-clock-tests/README.md) checks real timer/IOP cycle
credit while Ready native polling threads run.

Rate bounds alone do not prove universal guest observability. A long blocked
native call or a starved lower-priority waiter can legitimately miss fields;
noncooperative native functions must still return or checkpoint. Host sleeps
and dispatch can add jitter. These tests do not prove retail performance,
all scheduler workloads, or PS2 hardware timing. Linux validation is not
implied. All test source is GPL-3.0; see [`../LICENSE`](../LICENSE).

The baseline scheduled-offset fixtures intentionally leave video mode unconfigured
and exercise the prior 16,667 us fallback. Confirmed interlaced NTSC selection
uses 16,683,333 ns and 4,920,116 cycles through the actual GS mode API; separate
mode fixtures cover that selection and preserve pending End/alarm order.
Ordinary SDK waits sample CSR FIELD on continuation resumption.

SDK field return and registered output contract

This source-only package compiles the supplied canonical scheduler and kernel
interrupt syscalls unchanged and links the matching native runtime, IOP, SDK
stubs, and renderer dependency libraries. It renders no window, uses no owned
game data, and exercises the real ResetGraph, SyncV, SetVSyncFlag, and kernel
thread APIs. The supplied runtime must include the SDK contract change and its
build must match its current headers.

The ordinary interlaced wait returns current GS CSR FIELD at actual continuation
resume. Progressive mode returns 1; an explicitly fixed positive result remains
unchanged. Registered outputs are one-shot: the first is a completion flag and
the second preserves the full CSR sampled when the wait completed. Adjacent
sentinels and arbitrary other CSR bits verify write widths and masks.

A higher-priority authored guest suspends the Ready ordinary waiter across one
additional field. Its ordinary return must then reflect the resume-time FIELD,
while the registered output retains the earlier sampled CSR.

Negative controls are derived mechanically from those same canonical inputs in
the external build directory: inverse field return, replacing sampled CSR with
the tick counter, and removing resume-time sampling. The generator rejects
missing or ambiguous anchors before producing controls. No private captured
runtime, generated retail program, or copyrighted input is distributed.
Each control must reject exactly its intended assertion category and count;
an unrelated failure, successful control, crash, or timeout cannot pass it.

Configure on native Windows with Visual Studio 2022 and Python 3.8 or newer:

```powershell
cmake -S tests/gs-sync-contract-tests -B C:/scratch/gs-sync-tests -G "Visual Studio 17 2022" -A x64 -DPS2RECOMP_DIR=C:/src/PS2Recomp -DRUNTIME_BUILD_DIR=C:/build/runtime
cmake --build C:/scratch/gs-sync-tests --config RelWithDebInfo
ctest --test-dir C:/scratch/gs-sync-tests -C RelWithDebInfo --output-on-failure
```

Validation currently demonstrated by the separate private native Windows
fixtures: six immediate contract tests (165 positive checks plus old-contract
negative controls) and two delayed-resume tests (15 positive checks, two expected
wake-sampled control failures). This relocated source package is prepared for
validation against the final integrated runtime; its tests have not yet run.
Linux is not claimed: this fixture currently links Windows `.lib` dependencies.

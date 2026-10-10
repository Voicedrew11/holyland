# GS trace regression tests

This source-only package compiles the actual runtime `gs_trace.cpp` and its
public types. It uses synthetic register values, primitives and an eight-byte
snapshot callback. It includes no game assets, generated guest functions,
runtime binaries, captured private logs, or substitutes for the trace code.

```powershell
cmake -S tests/gs-trace-tests -B C:/KFIV-dev/test-builds/gs-trace -G "Visual Studio 17 2022" -A x64 `
  -DPS2RECOMP_DIR=C:/path/to/patched/PS2Recomp `
  '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG /Zi /fp:strict'
cmake --build C:/KFIV-dev/test-builds/gs-trace --config RelWithDebInfo --parallel 4
ctest --test-dir C:/KFIV-dev/test-builds/gs-trace -C RelWithDebInfo --output-on-failure -V
```

The nine default CTest entries cover:

- Combined and independent register, image-transfer and draw APIs, including
  calls without `onTick`: no records before `FROM`, both endpoints included,
  no records after `TO`, and recording remains closed after the upper bound.
- Distinct CRT1/CRT2 framebuffer and display fields in privileged snapshots,
  with duplicate `onTick` calls producing only one snapshot for a tick.
- Fractional XYOFFSET in both primitive bounds and per-vertex coordinates.
- Snapshot-callback VRAM dumping after `TO`, VRAM-only diagnostics, disabled
  diagnostics, and idempotent repeated shutdown.

Each scenario runs in a separate process because the trace configuration is
initialized once. Test files are written only under the supplied CMake build
directory. The fixture does not inspect every GS register, execute guest code,
or establish renderer/gameplay correctness.

An optional external `-DPRIOR_TRACE_SOURCE=C:/path/to/pre-fix/gs_trace.cpp` builds
the same tests against that source and adds six expected-failure controls for
range filtering, CRT2 output and fractional offsets. CTest marks these entries
`WILL_FAIL`; the old source is not bundled. No automatic source mutation or
destructive reset is performed.

Native Windows validation: MSVC 19.44, x64 RelWithDebInfo, `/O2 /fp:strict`.
The nine fixed scenarios passed 59 assertions with zero failures. The exact
pre-fix source failed 14 of 43 assertions across the six control scenarios,
and CTest passed 15/15 entries with those expected failures. The controls fail
because the old implementation emitted pre-FROM events, omitted CRT2 fields,
and truncated XYOFFSET fractions. Linux and a complete runtime build were not
run by this package.

The fixture is GPL-3.0-only and links GPL runtime source. See `LICENSE`.

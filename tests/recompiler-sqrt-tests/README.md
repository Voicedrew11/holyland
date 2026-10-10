# R5900 SQRT and RSQRT regression

Source-only synthetic opcodes go through the actual production R5900 decoder
and FPU translator. A build-time emitter writes C++ that is compiled and
executed against the runtime headers. No game code or disc data is used.

Raw `0x460C0004` must read Ft=f12, with Fs=f0 deliberately containing a different
value. A synthetic square/add/sqrt/divide ground-edge sequence proves a finite
unit normal when one axis is zero; the original produces zero length and infinity.
Further encodings distinguish every register field and exercise destination
aliasing. Runtime checks cover signed zero, denormal flushing, negative arguments,
live/sticky FCR31 flags, bounded exponent-255 inputs, RSQRT's numerator, overflow
and underflow, and unchanged ADD/SUB/MUL/ABS/MOV/NEG operations. A second executable
compiles the production translator with only the two old case bodies restored;
it reproduces the old operand defects.

Hardware reference: [PCSX2's EE FPU interpreter](https://github.com/PCSX2/pcsx2/blob/355608952714678b3c57832fb82dc6a42956ed25/pcsx2/FPU.cpp),
`SQRT_S` and `RSQRT_S`. This fixture tests operand and result handling; it does
not establish all PS2 FPU rounding behavior or retail progression.

Native Windows build, from the repository in PowerShell. First build the patched
tools as described in the [Windows guide](../../docs/windows.md). The source
checkout and tools build must be supplied explicitly; there are no workspace
defaults. Keep the fixture build directory outside the repository.

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$checkout = 'C:\KFIV-dev\PS2Recomp'
$tools = 'C:\KFIV-dev\tools-build'
$build = 'C:\KFIV-dev\test-builds\recompiler-sqrt-tests'
& $cmake -S tests/recompiler-sqrt-tests -B $build -G 'Visual Studio 17 2022' -A x64 `
  "-DPS2RECOMP_DIR=$checkout" "-DTOOLS_BUILD_DIR=$tools" -DTOOLS_CONFIG=Release
& $cmake --build $build --config Release --parallel 2
& $ctest --test-dir $build -C Release --output-on-failure
```

`PS2RECOMP_DIR` selects the patched checkout and its runtime headers.
`TOOLS_BUILD_DIR` selects the existing tools libraries; `TOOLS_CONFIG` defaults
to `Release` and must match those libraries. Configure and build the fixture
with that same configuration. No runtime build path is needed because the
generated execution checks use runtime headers only. Native Windows MSVC is
the verified platform. This fixture does not link the game runtime, renderer,
IOP, or other guest instruction execution.

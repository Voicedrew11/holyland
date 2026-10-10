# SPU compiler inlining comparison

This package compiles the supplied actual SPU2 implementation twice and
executes identical synthetic commands against both. MSVC uses `/O2 /Ob1`
for the reference and `/O2 /Ob2` for the candidate, retaining `/fp:strict`.
It verifies the small RelWithDebInfo source option for `iop_spu2.cpp` without
changing the mixer arithmetic, sample clock, DMA or IRQ behavior. It does
not test a broader IOP interpreter optimization.

`make_spu_reference.py` reads `iop_spu2.cpp` and its header from
`PS2RECOMP_DIR` and renames only the reference class and header include.
Generated copies and a source-hash manifest stay in an external build
directory. Source anchors are checked, production files are never changed,
and existing SPU attribution remains intact. No runtime source, game assets,
traces, logs or binaries are bundled here. New fixture sources are
[GPL-3.0](../LICENSE); the production source retains its RecompOne MIT notice.

The 24 scenarios exercise 0/1/12/24 voices per core, looping ADPCM,
Gaussian interpolation, envelopes, volume sweeps, pitch modulation, noise,
two-core routing, reverb with both power-of-two and other work areas,
normal DMA, AutoDMA refill/cancellation, fractional cycles, capture RAM
IRQs and reset callback retention. The comparator checks every emitted
PCM sample and chunk boundary, IRQ order/frame/status, visible MMIO,
statistics and all 2 MiB of final SPU RAM for each scenario. Side-effecting
PIO DATA reads are omitted from the register sweep; the existing
`audio-tests` regression is also compiled in both modes and tests PIO.

The category-aware negative control changes one observed candidate PCM
sample after the actual mixer callback. It must detect exactly one PCM
mismatch while chunk boundaries, IRQs, statistics, registers and RAM
remain exact. This corruption tests the comparator; it does not replace
the hardware implementation or alter production source.

## Build and verify

Use an external build directory. Python 3.9 or later and a C++20 compiler
are required; no existing runtime library or disc data is needed.

```powershell
cmake -S tests/spu-inlining-tests -B C:/KFIV-dev/test-builds/spu-inlining `
  -G "Visual Studio 17 2022" -A x64 -DPS2RECOMP_DIR=C:/KFIV-dev/PS2Recomp
cmake --build C:/KFIV-dev/test-builds/spu-inlining --config RelWithDebInfo --parallel 2
ctest --test-dir C:/KFIV-dev/test-builds/spu-inlining -C RelWithDebInfo --output-on-failure
```

On GNU/Clang the reference uses `-O2 -fno-inline` and the candidate uses
`-O2 -finline-functions`, both with `-fno-fast-math -ffp-contract=off`.
Those controls measure that compiler's inlining behavior and are not
identical to MSVC's `/Ob1` versus `/Ob2` distinction. The source supports
those compilers, but no Linux execution or speed result is claimed.

## Optional isolated timing

Correctness and timing are separate. Stop other gameplay, builds and
benchmarks. Alternate baseline and candidate runs with the same scenario.
The setup and RAM upload occur before timing; ten emulated audio seconds
are mixed, including the same PCM hashing callback. Compare all output
counts and hashes before interpreting wall time. The RAM/register exact
checks are performed by the separate correctness mode.

```powershell
$exe = 'C:/KFIV-dev/test-builds/spu-inlining/RelWithDebInfo/spu_inlining_tests.exe'
& $exe --benchmark baseline 3
& $exe --benchmark candidate 3
```

Scenario 3 exercises 48 voices across both cores with noise, volume sweeps
and reverb. Native Windows x64 MSVC 19.44 validation passed all four CTests,
including the PCM-only negative control. The exact comparison checked
221,664 assertions, 920,556 PCM samples and 50,331,648 RAM bytes;
both copies passed the existing audio regression's 607 assertions.
Six isolated alternating pairs reduced the synthetic mixer median from
0.674686 s to 0.544582 s, or 19.3%. All twelve runs produced the same
480,000 frames, 479,999 nonzero frames, 806,541 ADPCM blocks, 1,875 chunks
and PCM hash. This is a compiler-only SPU stress microbenchmark, not a
whole-game FPS measurement or a performance claim for another platform.

# Native VU catalog equivalence verifier

This source-only package generates its own 16 KiB VU1 program, synthetic
ELF and ordered MPG upload metadata in an external build tree. It runs the
candidate runtime's actual `tools/generate_vu_native.py`, then compiles that
catalog into a differential verifier against the pinned patch-39 runtime.
No game, disc, recording, generated program, copied runtime or binary is
included. Package sources are GPL-3.0-or-later; see [the tests license](../LICENSE).

The authored cases check exact registers and floating-point bits,
MAC/STATUS/CLIP flags, cycles, control state, all VU data bytes and ordered
PATH1 packets after execute/resume calls. Each intended compiled entry and
group-edge case must increment the native issue counter. Tests cover
short budgets, pipeline resumes, every valid normal/special upper opcode
and all 16 destination masks, additional source/destination aliases and
VF0, branches, Q/P, stores,
XGKICK, 16-pair group boundaries and code wrap, tracked code mutation,
both directions of unmarked stale-cache behavior, memory-owner changes,
callback mutations, misaligned PC, VU0, short buffers and untracked memory
fallback. The five negative controls require a category-specific mismatch
after compiled instructions actually issue; an arbitrary failure cannot pass.

This is exact equivalence to the supplied reference, rather than proof of
PS2 hardware accuracy or gameplay performance. The counters are enabled
only for this verifier; they are excluded from production builds.

## Inputs and build

Requires Python 3.10+, CMake 3.21+ and a C++20 compiler. Supply two distinct
`ps2xRuntime` directories explicitly. The candidate must include the native
backend, real `src/lib/ps2_memory.cpp`, complete headers and its generator
and `vu_native_input.py` module under `tools/`. The reference must be the
original PS2Recomp commit plus exactly patches 1–39. The five pinned VU
hashes and reference checkout instructions are in the
[base VU verifier](../vu-performance-tests/README.md).

Keep the build directory outside this repository and both runtime source
trees. Preparation rejects overlapping source/build paths and derives the
base verifier's actual sources into build scratch. It retains the base
package's fail-fast patch-39 hash checks. Generated authored inputs, catalog,
manifest, test-only runtime copies, logs and compiler output remain in the
build tree. A changed canonical VU source or generator triggers regeneration.

From this repository root in PowerShell:

```powershell
$candidate = 'C:\KFIV-dev\PS2Recomp\ps2xRuntime'
$reference = 'C:\KFIV-dev\PS2Recomp-reference39\ps2xRuntime'
$build = 'C:\KFIV-build\vu-native-tests'
cmake -S tests/vu-native-tests -B $build -G 'Visual Studio 17 2022' -A x64 `
  "-DVU_CANDIDATE_RUNTIME_DIR=$candidate" "-DVU_REFERENCE_RUNTIME_DIR=$reference"
if ($LASTEXITCODE) { throw 'Native verifier configuration failed.' }
cmake --build $build --config RelWithDebInfo --parallel
if ($LASTEXITCODE) { throw 'Native verifier build failed.' }
ctest --test-dir $build -C RelWithDebInfo --output-on-failure
if ($LASTEXITCODE) { throw 'Native verifier tests failed.' }
```

The candidate uses `/Ob2` and `/GL` by default with MSVC; the scalar reference
uses `/Ob1` and `/GL-`. Both use `/fp:strict`. Non-MSVC builds retain the base
verifier's no-fast-math, rounding and contraction settings and omit these
MSVC options. Native Windows MSVC 19.44 x64 is verified; Linux is unverified.

The derived base fixture links the actual upstream static-lift support
when present in the candidate, under strict floating-point options, and
forces `PS2X_VU1_LIFT=0` in CTest. These native catalog tests therefore
cover the dormant support/core boundary and bounded catalog execution.
They do not exercise enabled static lifts. The support header requires
x86 SSE4.1; GCC/Clang adds `-msse4.1` only to that support unit. Set
`PS2X_VU1_LIFT=0` for any direct verifier invocation as well.
The authored exact case passed 672,585 checks across 104,536 calls with
158,250 native-issued pairs. Each of 1,776 opcode/mask/alias entries must
issue through the compiled backend before resuming into a NOP drain that
checks committed arithmetic flags and pipeline timing. All five controls detected their intended category
after 11,984 native-issued pairs; all six execution CTests passed.
The six compiled execution tests comprise one exact authored case and five
register/flag/cycle/memory/packet corruption controls. The base package's
larger generic suite remains independently runnable.

`VU_NATIVE_GENERATOR` defaults to the supplied candidate's
`tools/generate_vu_native.py`; an explicit override supports a relocated
generator checkout with its adjacent `vu_native_input.py`. Generated catalog
code is compiled from build scratch without modifying the candidate source
tree. No runtime system compiler, emulator or game asset is required.

The separate parser CTest uses only authored ELF bytes. It checks ordered
overlap, wrapping, NUM=0, IRQ and address masks, exact payload boundaries,
both hash pins, metadata types and rejection before output publication.
Invalid input preserves an existing catalog and manifest.

`check_build_graph.py` additionally exercises the real `NativeVu.cmake`
module with an authored ELF. Supply `--runtime $candidate`, `--tools
"$candidate/tools"`, `--module "$candidate/cmake/NativeVu.cmake"`, `--cmake`
with the CMake executable, and `--work` with a new external directory.
It checks disabled builds without Python, partial-input rejection,
unchanged-build reuse, regeneration after all five VU sources, both tools
and both input dependencies, invalid-input failure despite stale outputs,
and deterministic output in a second directory. Logs remain in that work
directory. The 17 build/configuration cases passed on native Windows;
this does not imply Linux runtime validation.

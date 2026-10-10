# VU performance equivalence verifier

This standalone source-only package compares a candidate VU interpreter
against the exact VU source through KFIV-PC patch 39. It checks registers
and floating-point bits, MAC/STATUS/CLIP flags, cycle counts, control state,
all VU data bytes and every emitted PATH1 packet after each execute/resume
call. An equivalence pass is a regression check against that reference;
it does not establish PS2 hardware accuracy or full-game performance.

The package contains synthetic instructions only. It needs neither a disc
nor generated game C++, and bundles no reference runtime source, recording,
native state, executable or result log. Its code is GPL-3.0; see
[the tests license](../LICENSE).

## Inputs and reference provenance

Supply two **separate** `ps2xRuntime` directories explicitly:

- `VU_CANDIDATE_RUNTIME_DIR`: the runtime under evaluation, including the
  real `src/lib/ps2_memory.cpp` and complete runtime headers.
- `VU_REFERENCE_RUNTIME_DIR`: a PS2Recomp checkout at
  `c5a9d02573410a2085a4b4b831b0b68ba3515440`, with exactly KFIV-PC patches
  `0001` through `0039` applied, including patch 39 itself.

[reference39-hashes.json](reference39-hashes.json) pins the five VU files
used by the reference: its public header, core, upper, lower and detail
header. Preparation checks every hash before writing any copies. It accepts
only CRLF/LF conversion by checking the pinned SHA256 after replacing CRLF
with LF; the original frozen Windows byte hashes are also retained in the
manifest. It does not accept other whitespace or source changes. The
build manifest records both raw and normalized input hashes and hashes
of all generated source copies.

From the KFIV-PC repository root, create a fresh reference checkout in
PowerShell. Use an unused destination; these commands do not reset an
existing checkout:

```powershell
$reference = 'C:\KFIV-dev\PS2Recomp-reference39'
if (Test-Path -LiteralPath $reference) { throw 'Choose an unused reference directory.' }
git -c core.autocrlf=false clone https://github.com/ran-j/PS2Recomp.git $reference
if ($LASTEXITCODE) { throw 'Clone failed.' }
git -C $reference checkout --detach c5a9d02573410a2085a4b4b831b0b68ba3515440
if ($LASTEXITCODE) { throw 'Pinned checkout failed.' }
$patches = @(Get-ChildItem -LiteralPath patches -Filter '*.patch' |
  Where-Object { $_.Name -match '^(00[0-3][0-9])-' -and [int]$Matches[1] -ge 1 -and [int]$Matches[1] -le 39 } |
  Sort-Object Name)
if ($patches.Count -ne 39) { throw 'Expected the original 39 patches.' }
for ($index = 0; $index -lt 39; $index++) {
  if (-not $patches[$index].Name.StartsWith(('{0:D4}-' -f ($index + 1)))) {
    throw 'The first 39 patches are missing or duplicated.'
  }
  git -C $reference apply --check -- $patches[$index].FullName
  if ($LASTEXITCODE) { throw "Patch check failed: $($patches[$index].Name)" }
  git -C $reference apply -- $patches[$index].FullName
  if ($LASTEXITCODE) { throw "Patch application failed: $($patches[$index].Name)" }
}
```

Do not use the full patch-series installer to construct this reference
when later patches are present. The five hash checks are the final test
that the supplied reference VU source is the intended version.

## Native Windows build and test

Requires Python 3.10 or newer, CMake 3.21 or newer, and Visual Studio 2022
Build Tools with the x64 C++ toolchain. Keep the build directory outside
this repository and both runtime source trees; preparation rejects source
tree output. No prebuilt PS2Recomp library or window/graphics dependencies
are required.

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$candidate = 'C:\KFIV-dev\PS2Recomp\ps2xRuntime'
$reference = 'C:\KFIV-dev\PS2Recomp-reference39\ps2xRuntime'
$build = 'C:\KFIV-dev\test-builds\vu-performance-tests'
& $cmake -S tests/vu-performance-tests -B $build -G 'Visual Studio 17 2022' -A x64 `
  "-DVU_CANDIDATE_RUNTIME_DIR=$candidate" "-DVU_REFERENCE_RUNTIME_DIR=$reference" `
  -DVU_VERIFY_OB2=ON -DVU_VERIFY_LTCG=ON
if ($LASTEXITCODE) { throw 'Configure failed.' }
& $cmake --build $build --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Build failed.' }
& $ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Verification failed.' }
```

MSVC compiles all units with `/fp:strict`. Reference VU units explicitly
use `/Ob1 /GL-`. Candidate VU units also default to `/Ob1 /GL-`; the
`VU_VERIFY_OB2` and `VU_VERIFY_LTCG` options enable `/Ob2` and `/GL` on
those three candidate units and optional static-lift support unit. LTCG adds `/LTCG` at link. CMake's
global Release inlining default cannot silently alter the reference.
Disable both options to compare source changes with matching compiler
options. Do not add fast-math, floating-point contraction or whole-target
IPO. The non-MSVC configuration disables contraction and fast-math, but
does not imply native Linux runtime validation.

When the actual candidate core calls `runLifted`, preparation also copies
and hashes its real `ps2_vu1_lift.cpp` and header into build scratch. That
support unit is linked with strict floating-point options. On GCC/Clang
x86, only that unit adds `-msse4.1` for the upstream header's intrinsics;
other architectures cannot compile this particular upstream support code.
CTest sets `PS2X_VU1_LIFT=0` for every verifier process. This checks the
actual dormant support/core boundary while evaluating the bounded native
catalog and interpreter. It does not validate enabled static lifts, their
unbounded execution budgets, or their arithmetic. Set the same environment
value when invoking the verifier executable directly.

CTest runs the full synthetic equivalence suite, direct decoded-descriptor
comparisons and six corruption controls. Each control flips exactly one
candidate register, flag, cycle counter, VU data byte, captured packet byte
or exported read-slot value. Its wrapper requires exit
code 1 and the matching `MISMATCH ... category=...` diagnostic. A crash,
an earlier mismatch in another category or undetected corruption fails
the test.

The descriptor test compares 6,389,760 pairs from the two **actual** opcode
decoders. It covers both VU units, every upper/lower selector family,
register aliases/zero/edge indices, masks and control bits. It compares
instruction words, pipeline/resource and control flags, upper/lower alias
handling, exact ordered read/write slots, latencies and branch bookkeeping.
The current decoder's tight bounds of 17 read slots and 9 writes must both
be reached and never exceeded. Native descriptor sizes are reported to
make cache-layout changes reviewable; size alone does not prove speed.
Test-only exports are inserted into the derived scratch headers/core,
while the pinned patch-39 baseline hashes remain mandatory and unchanged.
Run just this check with `kfiv_vu_performance_verify --metadata`.

Native Windows MSVC 19.44 x64 Release validation of the combined compact-decoder
and queued-STATUS candidate passed all nine tests, including the optional
private replay. The synthetic suite passed 1,562,391 exact checks across 176,890
calls; direct metadata comparison passed all 6,389,760 pairs and reported
180-byte reference and 52-byte candidate descriptors. All six expected
corruption failures were detected. The private replay passed 39,999 checks
across 4,000 contiguous calls. Candidate VU units used `/Ob2 /GL`; all units
used `/fp:strict`. These checks establish equivalence for the tested inputs,
without a whole-game performance claim.
Source preparation accepted identical LF input and rejected a changed
reference, shared input directories and source-tree output before writes.
No Linux runtime check was performed for this package.

Coverage includes arbitrary IEEE floating-point encodings, signed zero,
NaN payloads, FLT_MIN/MAX boundaries, product-sum cancellation and sticky
flags; every destination mask; VF/VI/ACC hazards; Q/P issue, throughput
and visibility; small execution budgets and one-cycle resumes; active
XGKICK backpressure and packet restart; same-buffer code mutation and
I-bit decoding; occupied flag/store slot reuse, arbitrary public high STATUS
bits, simultaneous Q and flag commits, and FSSET/FCSET cancellation.

## Actual and substituted components

The candidate's **real PS2Memory implementation** supplies allocation,
VU code/data storage, code-generation tracking and its packet callback.
Both actual VU interpreters run their scheduler, instruction arithmetic
and XGKICK state machine. Reference names are mechanically changed to
`VU1InterpreterRef`/`VU1StateRef` and its header is renamed, exclusively
inside build scratch, so both implementations can link in one process.

`unrelated_stubs.cpp` substitutes only GS construction/raster dispatch,
unused VIF0/VIF1 dispatch and the downstream GIF arbiter. The real memory
callback captures PATH1 packets before those stubs; comparisons cover
their full byte payloads. This fixture does not rasterize packets or test
the window, EE scheduler, audio, VIF decoding or GS backend.

Preparation also inserts build-only state restoration for an optional
replay and wrappers around the actual upper operand-normalization helper.
When the candidate core contains `classifyFmacDoubleLanes`, its wrapper
calls that production SSE2 helper; otherwise it uses the literal scalar
patch-39 classifier, also used for the reference wrapper. The latter is
an auxiliary classification check, while complete instruction execution
still compares the actual candidate and reference implementations. The
generated manifest identifies which classifier wrapper was selected.
No test accessor or wrapper is inserted in either input source tree.

## Optional private replay and timings

A privately retained contiguous `KFIVVU39` version-1 recording can be
supplied explicitly during configuration:

```powershell
& $cmake -S tests/vu-performance-tests -B $build `
  "-DVU_CANDIDATE_RUNTIME_DIR=$candidate" "-DVU_REFERENCE_RUNTIME_DIR=$reference" `
  '-DVU_PRIVATE_REPLAY=C:\KFIV-private\vu-recording.bin'
& $ctest --test-dir $build -C Release -R vu_current39_private_replay --output-on-failure
```

Or invoke `Release\kfiv_vu_performance_verify.exe
--replay=C:\KFIV-private\vu-recording.bin` directly. A recording must
start at an execute boundary and contain contiguous calls. Its native
state layout must match this compiler/architecture and the patch-39 state
ABI. The parser checks the magic, version, state size, payload dimensions,
packet bounds and continuity, and compares recorded state, data and
packets in addition to candidate/reference equivalence. Such recordings
contain game-derived input: keep them, generated source copies, manifests,
binaries and result logs outside Git. This package has no recorder and
does not obtain or redistribute recordings.

Replay reports nanoseconds accumulated around VU calls with alternating
candidate/reference order. For performance evidence, use an otherwise
idle machine, repeat runs with matching options and compare against the
same fixed reference. These timings exclude memory setup and comparison
overhead; they do not measure whole-game frame rate. Run genuine gameplay
cadence and input/attack timing checks separately before claiming restored
real-time gameplay.

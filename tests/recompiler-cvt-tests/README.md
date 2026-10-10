# EE CVT.W.S generated execution

This source-only fixture compiles the actual R5900 decoder and COP1 translator,
emits synthetic opcodes, and compiles and executes their output against the
production runtime macro header. No game data, retail generated functions,
frames or poses are included.

The EE conversion truncates toward zero, saturates by the input sign outside
the signed 32-bit range, and preserves FCR31. The primary reference is
[PCSX2's EE interpreter CVT_W](https://github.com/PCSX2/pcsx2/blob/355608952714678b3c57832fb82dc6a42956ed25/pcsx2/FPU.cpp#L254-L258).
Exponent-255 patterns saturate according to their raw sign; the fixture includes
positive/negative infinities and quiet/signaling NaN patterns.

Coverage includes every FPR source/destination alias, distinct source/target
registers, ignored Ft fields, signed zero, subnormals, fractional and tie inputs,
the largest in-range floats, exact +/-2^31 and neighbors, overflow, unchanged
source bits/flags/accumulators, all four native host rounding modes, and single
argument evaluation. A synthetic angle-reduction sequence uses the actual
translated ADD/DIV/CVT/MUL/SUB operations. An independent mathematical Taylor
polynomial illustrates how wrong reduction can reverse positive rotations;
this final approximation is not execution of a production VU implementation.

The negative control restores only the former `nearbyintf` conversion macro in
a build-local header. It runs the same generated instructions and must fail.
Its overflow/NaN cases are skipped because the former host cast has undefined
behavior for those inputs. The corrected executable runs all cases.

Native Windows MSVC is the verified configuration. Set paths as described in
[the shared fixture guide](../README.md):

```powershell
cmake -S tests/recompiler-cvt-tests -B <private-build> -A x64 `
  -DPS2RECOMP_DIR=<patched-checkout> -DTOOLS_BUILD_DIR=<tools-build>
cmake --build <private-build> --config RelWithDebInfo --parallel 2
ctest --test-dir <private-build> -C RelWithDebInfo --output-on-failure
```

Verified on native Windows x64 with MSVC 19.44, RelWithDebInfo and `/fp:strict`:
199,956 production checks pass; restoring the original macro yields 920
failures across 133,316 checks. Both CTest entries pass, including the expected
negative-control failure. Linux execution has not been verified.

These checks establish the conversion and code-generation behavior. They do
not establish accuracy of every EE arithmetic operation or a full playthrough.

This package is GPL-3.0-or-later; see [the tests license](../LICENSE).

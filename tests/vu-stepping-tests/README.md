# Cooperative VU exact comparator

This source-authored fixture compares current candidate VU arithmetic, pipeline,
decoded hazards and resumable jobs with the separately supplied patch-39 scalar
reference. It preserves full state, cycles, memory and PATH1 equality, quantum
bounds, stopping/draining, callback mutation, resets, FP restoration and
legacy/job mutual exclusion. Meaningful category corruption controls remain.

Supply `VU_CANDIDATE_RUNTIME_DIR`, `VU_REFERENCE_RUNTIME_DIR` and
`VU_MEMORY_RUNTIME_DIR` explicitly. The latter is the checked pre-cooperative Q
`ps2xRuntime` allocator/header source tree. Its actual allocation, code-generation
tracking and PATH1 callback run; authored stubs substitute unrelated VIF and GS
rasterization. It is isolated VU proof, not full cooperative-runtime layout proof.
No runtime source copy is bundled. The W61 linked package supplies actual W
allocator/parser/scheduler/admission proof separately.

Configure in an external `-B` directory, build and run CTest serially. Set
`VU_PRIVATE_REPLAY` only for a privately owned recording; none is distributed.
MSVC uses strict FP; GNU/Clang use rounding-aware/no-fast-math/contract-off.
The actual optional static-lift TU is linked; lift is disabled in ordinary tests
and the explicit host-job bypass sentinel runs with it enabled. The relocated
graph passed all 11 CTests on native Windows x64/MSVC 19.44, RelWithDebInfo.
Linux/compiler coverage requires fresh execution. GPL-3.0-or-later.

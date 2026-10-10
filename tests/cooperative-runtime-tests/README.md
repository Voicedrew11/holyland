# Actual-runtime cooperative clock and presentation admission

Set `PS2RECOMP_DIR` explicitly. This standalone graph builds/links the real
`ps2_runtime` target and its host, IOP, FFmpeg and enabled Vulkan dependencies;
every allocator, producer, scheduler and consumer uses the current W headers.
It contains the preserved 49 valid clock/physical-phase assertions, eight actual
cooperative scheduler modes and four production admission modes (61 accepted).
Proof-only friend declarations expose existing scheduler fields without changing
class layout. DMA/VU progress, completion, reset, cancellation, thread/token
reuse, blocked input and physical field snapshot deferral remain exercised.

`COOPERATIVE_GS_BACKEND=cpu|vulkan` is explicit (default CPU, no auto fallback).
Windows original W61 evidence used Vulkan. The admission fixture deliberately
installs a counted CPU raster backend to observe real runtime latch policy; it
is not Vulkan pixel-equivalence evidence. The separate presentation package
provides same-Vulkan-backend full-image/VRAM comparison.

The original Q no-floor assertion remains registered, disabled and labelled
`obsolete-q-contract` by default. Its zero-field-during-native-work expectation
is incompatible with W physical phase; it is not treated as a passing control.
`COOPERATIVE_ENABLE_OBSOLETE_Q_CONTROL=ON` permits explicit legacy investigation.
Timer undercredit, bulk-credit, handoff and helper controls remain active.

Native Windows W61 passed against the fully rebuilt frozen production archive.
The relocated graph also passed all 61 active CTests after rebuilding the actual
runtime from the supplied W source on native Windows x64/MSVC 19.44,
RelWithDebInfo, with Vulkan selected. The obsolete Q assertion remained disabled.
The source-only runtime build supplied no private ELF/catalog inputs; compiled
native paths are covered separately by the authored native VU package. The
original frozen-archive proof used the actual retail runner's runtime archive.
GNU/Linux and hardware-backend results must be reported separately.
GPL-3.0-or-later.

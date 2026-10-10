# Fixed game updates and independent rendering

Patches 0057–0058 add an opt-in performance mode. It preserves the original
US game's NTSC cadence, **30000/1001 updates per second** (29.970, conventionally
called 30 Hz), while presenting additional interpolated graphics frames.
No assets or generated game functions are replaced.

## Running

Build the complete patch series with the bounded native VU catalog enabled
as described in [Windows setup](windows.md) and [optional PGO](windows-pgo.md).
The new mode is selected explicitly; the existing launchers retain their settings.

```powershell
./scripts/windows/Run-KFIVPerformance.ps1 -GameDirectory C:\KFIV-PC -RenderFps 120
```

An executable outside that game directory can be selected with `-Executable`.
`-OriginalFrames` retains fixed simulation timing but disables interpolation.
`-RenderFps 0` removes the host presentation limit; it does not accelerate
the game. A requested limit is not a guaranteed achieved frame rate.

On Linux, after building a native runner:

```sh
PS2X_PRESENT_FPS=120 ./scripts/04-run-performance.sh
```

`PS2X_RUNNER` and the existing game-directory configuration still apply.
Use `PS2X_FRAME_INTERPOLATION=0` to retain original graphics frames. A supported
Vulkan GPU is needed for the measured Windows rendering performance; CPU GS
remains available with `PS2X_GS_BACKEND=cpu`.

## What changes

- A game-identity and instruction-signature guarded override replaces the
  original timer/FIELD polling limiter with rational host deadlines. It yields
  to the existing EE scheduler, then resumes the original buffer-swap and
  return sequence. Hardware fields, IOP service and guest callbacks continue
  during that wait. Combat counters, input handling, physics and animation
  updates are still executed by the original game code.
- Bounded VU execution reuses an admitted native descriptor and specializes
  ordinary FMAC flag handling. Boundary/cancellation cases keep the exact
  arithmetic path. MSVC x64 saves/restores the full MXCSR environment directly;
  other platforms retain their portable floating-point environment path.
  Cooperative VIF service uses a bounded 2048-cycle quantum.
- The original graphics backend still receives every game command. Two
  independent presentation backends replay owned commands: one maintains the
  original sequence; the other renders intermediate triangle positions. Their
  video memory never feeds back into the game. Completed frames are handed to
  the render thread without an extra GPU readback on the game thread.
- Only perspective-textured triangles with compatible material/UVs and an
  unambiguous nearby predecessor interpolate. HUD sprites, new/clipped geometry
  and rejected matches retain their original frame positions. Before using a
  frame, its unmodified replay must exactly match the canonical presentation.
  Mismatches use the canonical image. Command and frame queues are bounded;
  overflow disables interpolation for that session instead of growing forever.
  Non-game timer resets suspend capture immediately. If the original game
  enters another loop without that signal, a 100 ms missing-boundary timeout
  returns to live presentation. The next gameplay boundary checkpoints fresh
  memory, so inventory, pause and loading do not retain an old game picture.

Interpolation introduces approximately one game update of visual delay. It
does not change simulation timing, but that presentation latency is a real
tradeoff. Matching is conservative and heuristic; not every object is smooth,
and endpoint equality alone cannot prove every intermediate image is correct.
Long stalls discard accumulated deadlines after four periods. This mode cannot
promise 30 updates/s when the host takes longer than a frame to execute the game.

## Native Windows measurements, 2026-10-10

Same i9-13900KF / RTX 4090 / 64 GiB machine, native Windows, Vulkan, strict
MSVC 19.44 arithmetic, bounded native VU catalog, upstream static lift disabled.
The retained binary uses `/O2 /Ob2 /DNDEBUG /Zi /fp:strict` and MSVC PGO.
Use that `CMAKE_CXX_FLAGS_RELWITHDEBINFO` value when reproducing this candidate;
the older general Windows guide still documents its historical `/Ob1` build.
Its final link reported profile optimization
for 4970/5317 functions and all profiled dynamic instructions, with no mismatched
profile counts. The training profile preceded the final helper-inlining edits;
this is not a new profile trained on every final function. See [PGO](windows-pgo.md).

The opening area's original loop, attack animation and cooldown were observed
through read-only process memory at 5 ms intervals. These are game-update
measurements, not estimates from window FPS or display-buffer flips.

| Candidate | Updates/s | Original 15-update attack windup |
|---|---:|---:|
| Previous PR foundation, locally reproduced PGO build | 26.933 | 0.595 s |
| Fixed deadlines, original graphics frames | 30.000 | 0.500 s |
| Interpolated build before transition recovery, 120 FPS limit | 29.966 | 0.500 s |
| Same build, 300 FPS limit | 29.967 | 0.505 s |
| Final build after inventory and pause/resume, 120 FPS limit | 29.733 | 0.500 s |

The first interpolated 30-second capture observed 899 updates in 30.00037 seconds.
The attack animation and cooldown values advanced through their original
sequences. Graphics presentation was roughly 80–90 frames/s in this scene,
with a requested 120 FPS limit. A prior PR run recorded 28.20 updates/s under
different conditions; the 26.933 row is the local comparison used here, not
a revision of that older measurement.

Raising the limit to 300 retained 899 updates in 30.00000 seconds and the same
contiguous attack/cooldown sequences. Actual graphics stayed around 80–100
frames/s in that scene; the higher limit did not accelerate gameplay. One
animation/loop label alignment check in this second capture was false despite
contiguous animation values, reflecting the non-atomic sampling limitation.

The final build fixes capture suspension/restart across non-game loops. Inventory
and pause were visually checked, and interpolation resumed with zero endpoint
fallbacks. The subsequent 30-second capture observed 892 updates: some frames
still overran the target despite the correct 0.500-second attack windup.
Thus this is a substantial improvement and working independent presentation,
**not an unconditional locked-30 claim**. The final capture also retained
contiguous animation/cooldown values with a non-atomic loop-label mismatch.

Read-only sampling is not an atomic guest snapshot and adds some overhead.
Timing brackets are about 5 ms plus host scheduling jitter. Animation windup
is the original hit-window threshold, not confirmation that an enemy received
damage. An average near 29.97 does not prove every frame met its deadline.
No whole-game performance guarantee follows from this opening-area capture.

## Correctness checks

Native Windows and Ubuntu both passed:

- Three new fixtures using the rational pacer, actual runtime scheduler and
  actual CPU renderer. They check continued physical fields and callbacks,
  original continuation/register handling, re-registration, teardown, a real
  intermediate image different from both endpoints, and unchanged guest VRAM.
  They also check non-game suspension, missing-boundary fallback and fresh
  capture after returning from another scene, on both platforms.
- Seven compiled authored VU differential CTests and seven bounded native VU
  stepping CTests. The additional stepping run covered 334,058 checks over
  12,208 calls. Comparisons include flags, registers, cycles, memory and packets.

Native Windows retail verification additionally ran more than 2400 completed
frames with camera turning and attack input using `PS2X_INTERPOLATION_VERIFY=1`.
Canonical replay matched the actual game backend's complete RGBA image and
all 4 MiB of VRAM on every checked frame, without fallback. That expensive
verification mode is disabled in performance measurements. Subsequent retained
runs also reported zero endpoint fallbacks.
The final transition-aware binary then completed another verification session
with inventory, pause/resume, turning and attack input: more than 2000 completed
frames matched complete RGBA and VRAM, with zero verification failures or
endpoint fallbacks and a normal exit.

The older foundation's broader tests and gameplay observations remain recorded
in [Windows validation](windows-validation.md); they were not all rerun for this
change. A full playthrough, every later scene and real save/load remain unverified.

## Ubuntu and the owner's merged work

The comparison point is upstream `8719d72d3f258cfc194ac23a5af66889bcc38ef7`,
the merge of [owner PR #11](https://github.com/Voicedrew11/holyland/pull/11).
It was already an ancestor of this branch before these edits. Its VU census,
disassembler, opt-in static lift, lifted-source staging and handoff notes remain
intact. Patches 0040–0041 are unchanged.

The bounded native catalog in patch 0047 is a separate implementation from the
owner's `PS2X_VU1_LIFT` experiment. These tests keep `PS2X_VU1_LIFT=0` and do not
claim to validate that experimental path. The new Ubuntu changes address the
newer bounded-catalog implementation and its tests:

- Give generated catalog includes an explicit source include directory; GCC
  does not use MSVC's enclosing-include search behavior.
- Explicitly capture the constexpr FMAC descriptor in generated lambdas.
- Narrow Mersenne Twister results to `uint32_t` before `bit_cast<float>` in
  fixtures; Linux's `uint_fast32_t` result type can be 64 bits.
- Link the new fixture to native Linux libraries and use portable environment
  setters. No Windows object or PGO profile is used by the Linux build.

Ubuntu 26.04 under WSL2 used GCC 15.2 and CMake 4.2.3. The complete native Linux
runner was compiled on ext4 from the same runtime and privately generated game
sources, with a freshly generated native VU catalog. Owned disc files were
read through the Windows filesystem mount.

WSL's available hardware graphics route was Mesa Dozen 26.0.3 over Direct3D 12
on the RTX 4090, not the native Linux NVIDIA Vulkan driver. The interpolated
game run failed before gameplay with `Failed to end command buffer`,
`D3D12: Removing Device`, compute-pipeline errors and process signal 11.
Passing Linux source tests therefore does not establish a working Vulkan
game session or the Windows performance figures on Linux.

A control run with interpolation disabled failed at the same initial shaders
and device removal. The failure is therefore not specific to the additional
presentation renderers; native Linux Vulkan remains unverified here.

The CPU-renderer fallback completed actual Ubuntu game sessions and
30-second, 6001-sample captures with normal exits. Before transition recovery
it measured **23.765 updates/s** and a **0.650 s** original 15-animation-frame
windup. The final build measured **21.700 updates/s** and **0.680 s**.
Animation and cooldown value sequences were contiguous. Animation/loop label
alignment checks were false (the final windup had 16 observed loop labels);
those reads are not atomic, so it is not evidence of a skipped frame. CPU
replay could not keep up and correctly hit the bounded queue fallback to
original presentation. This is functional Linux execution, **not a successful
Linux 30 Hz / high-FPS result**. The final transition-aware build and all three
new fixtures were rebuilt and rerun on Ubuntu. No system driver or security
policy was changed.

The final 58-patch series was applied to a fresh pinned checkout, first in
tools-only mode and then in full. Repeated application verified all 58 patches
and 83 managed files without changing them.

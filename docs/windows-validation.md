# Native Windows validation (2026-10-09)

The USA boot ELF `SLUS_203.18` was statically recompiled and run as a native
x64 executable on Windows 11 with MSVC 19.44. The IOP and VU1 remain
interpreters in the supporting runtime. The earlier validation below uses
the CPU GS; optional Vulkan GS is now verified separately in
[Vulkan rendering](vulkan.md), including native timing and remaining limits.
The verified build was `RelWithDebInfo`, `/O2 /Ob1 /DNDEBUG /fp:strict`,
with FFmpeg enabled and matching shared runtime DLLs.

## Generation and source fixes

The final generation processed 28,429 functions, recompiled 28,161 and used
268 SDK stubs. It reported 1,352 JR/JALR fallback warnings, zero errors,
zero decode failures, zero skipped functions and zero unhandled
instructions. The diagnostics counted 119,442 registered fallback entries.
The older 999-warning report was a historical
baseline, before the final residual-entry regeneration.

The earlier CPU verification covered 26 patches: the existing 0001–0014 plus twelve
source-exported additions, 0015–0026. Patch 0027 now adds the optional Vulkan
backend separately. The earlier additions change 43
source/license/build files across Windows, controls, IOP file completion,
audio, clocks, EE scheduling/callbacks,
MPEG/IPU, the generator, GS depth testing, sprite alignment, source-height
decoding and diagnostics.
Audio patch 0022 accounts for
31 of those files.
The focused generator change is proposed upstream
in [PS2Recomp PR #279](https://github.com/ran-j/PS2Recomp/pull/279).
The disabled-depth fix is proposed separately in
[PS2Recomp PR #280](https://github.com/ran-j/PS2Recomp/pull/280).
The paired sprite-coordinate fix is proposed separately in
[PS2Recomp PR #281](https://github.com/ran-j/PS2Recomp/pull/281).
Patch `0022` remains necessary for the pinned `c5a9d02` base until that pin
moves to a version containing the fix. Rebuilding the tools and regenerating
`register_functions.cpp` is required.

## Retail integration run

The earlier depth-fixed build reached tick 10,600 and exited normally
with code 0 in 248.218 seconds, without timeout or forced termination and
with private test cards. Title/New Game/brightness selection entered the
full opening movie. Captures show its pictures and audio, natural movie
completion, and the first gameplay area. Interlace combing remains visible
in some frames; this is not a complete rendering or A/V accuracy check.

In that run, W at tick 9,000 changed player coordinates from
`(-5656, 0, -10718)` to `(-5731.044, 0, -10365.910)` after gameplay became
active. Inventory entry/back and pause/resume also completed. Space at
tick 9,800 produced Square held/edge bits `0x8000` in both raw pad and
guest input state. This proves attack input delivery; attack animation,
damage and combat outcomes were not independently measured.

The old black picture was a GS depth-test defect. Decoded RGB pixels
reached texture memory, but `TEST.ZTE=0` still applied the stored
`ZTST=NEVER` comparison. Patch 0023 bypasses depth comparison when ZTE is
disabled and prevents depth writes. The original movie upload, textured
sprite and display path now draw the picture without a replacement overlay.

Audio counters showed zero underruns during the opening movie. After the
transition into 3D gameplay, the final sampled counters recorded 256
underrun frames and zero dropped frames. This run does not establish
underrun-free gameplay audio.

A separate smoke run used the installed executable and only host keyboard
input for title/New Game/brightness. It displayed the opening forest and
exited normally at tick 2,520 in 44.338 seconds, with no timeout or forced
termination. Its stereo 48 kHz PCM capture contained 2,104,320 frames
(43.84 seconds), peak 22,379 and zero clipped samples; sampled counters
showed zero underruns and drops. This shorter run verifies installed movie
output, rather than natural completion of the entire movie.

### Gameplay strip alignment

The sprite-corrected preview reached tick 4,500 and exited normally with
code 0 in 128.879 seconds, without forced termination. At the same initial
player position, its gameplay captures show continuous wall textures and
sky where the prior renderer showed repeated vertical strip boundaries.
The baseline run had full GS tracing enabled and took 194.588 seconds;
these durations are not a renderer performance benchmark.

The traced feedback sprites reversed both coordinate endpoints. The old
rasterizer sorted XY without sorting the matching UV components, mirroring
each 64-pixel strip. It also truncated fractional XYOFFSET and UV, excluded
the final pixel at a 63.5 endpoint, and sampled sprites half a pixel away
from the original GS coordinate. Patch 0024 preserves paired endpoints,
fractional coordinates, ceil-exclusive coverage and integer GS sampling;
STQ sprites use the second vertex's flat Q. The existing bilinear
half-texel offset is retained. This fixes rasterization of the original
feedback passes without a postprocessing blur or a new texture-cache policy.
The rules follow the pinned primary
[PCSX2 sprite rasterizer](https://github.com/PCSX2/pcsx2/blob/355608952714678b3c57832fb82dc6a42956ed25/pcsx2/GS/Renderers/SW/GSRasterizer.cpp#L1088).

Patch 0025 affects opt-in diagnostics only. Every trace event respects
the selected inclusive tick interval; out-of-range register, transfer and
draw events no longer create large unwanted traces. Privileged-state
records include CRT2, and primitive coordinates preserve fractional XYOFFSET.
This is separate from the gameplay image correction. Other presentation
and renderer accuracy issues remain outside these fixes.

A later installed check exposed a separate image crop: gameplay uses
`DISPLAY2.DH=895`, `MAGV=0` and interlaced FRAME mode. The old size decoder
limited 896 display lines to the 512-row host maximum before halving for
FRAME mode, so it read only 256 source rows and falsely doubled them to
512. Patch 0026 converts the encoded height first, retaining all 448 source
rows; the existing doubling guard then presents the full-height gameplay
image directly. The opening movie's 224-row field path remains unchanged.
This correction does not replace the general weave implementation.

The final installed build then reached tick 4,800 and exited normally with
code 0 in 137.034 seconds. Its 81 captures are 640×448 and show the complete
gameplay scene and HUD, with continuous walls and sky. Keyboard movement,
camera turning, inventory entry/back and pause/resume were checked. Opening
movie captures retained their visible 640×448 output. The installed executable,
all 28 matching DLLs and the desktop shortcut used this verified build.
An additional snapshot run exited normally at tick 4,500 in 122.400 seconds.
Its 640×448 output matches the selected framebuffer across all 448 rows:
99.96233% of pixels are exact, with maximum RGB difference two. Both row
parities align, and all bottom 192 rows match exactly. The small remaining
differences are consistent with snapshot/animation timing. This establishes
the observed source-row coverage and alignment, rather than complete PS2
hardware equivalence.

Audio showed zero sampled underruns through the movie; after pause and 3D
gameplay, cumulative counters recorded 2,304 underrun frames and zero dropped
frames. This is separate from the earlier depth-fixed run's 256-frame result
and does not establish underrun-free gameplay audio or complete audio accuracy.

### Earlier audio and keyboard integration

Before the GS depth fix, a combined run reached tick 10,500 and exited
normally with code 0 in 233.323 seconds, without timeout or forced
termination and with private test cards. Title/New Game/brightness
selection entered the full opening
audio path. The MPEG stream accepted all 2,772 retail pictures, but the
presented opening image remained black in that earlier build.

The opening ended naturally at tick 7,483, about 125.64 seconds into the
run, before scripted Enter input at tick 9,300. The demux consumed the exact
87,556,100-byte PSS program; the CD producer supplied 87,558,144 bytes,
including 2,044 bytes of sector padding. Accepted program-end and the
original cleanup returned to the real area loader and first gameplay area.
The sampled global trace did not capture the exact remote STOP command,
so it does not establish that particular call.

The program-end fix commits MPEG B9 only after preceding callback input
has been accepted. Unread CD padding no longer prevents completion, and
bytes after B9 invoke no callbacks. Sequence-end B7 alone cannot complete
the program: a later accepted B9 or physical producer EOF is still needed.
The decoded reference count is published at the original SDK work-arena
word `work + 4`, allowing its original reference-empty getter to finish.

The host input path supplied W at tick 8,000, Space at 8,300, inventory
entry/back at 8,500/9,100, pause at 9,300 and resume at 9,603, followed by
Space at 9,800. The first two inputs were during the original first-area
intro before the gameplay/capture gate became active. Frame inspection
confirmed the first-area HUD, inventory and a genuine PAUSE overlay.

A follow-up run used the exact installed executable and only host keyboard
input for title/New Game/brightness: Enter at 1,100, F at 1,500 and F at
1,900. It reached natural movie completion, the first area and inventory,
then exited normally at tick 8,800 in 178.336 seconds with private cards.
The movie itself contains about 92 seconds of audio; loading and the
original first-area intro add time before free movement. The earlier black
opening picture could look like a hang, especially with PC audio muted.
Enter sends the game's original Start input to skip the movie after it
begins; normal movie/audio cleanup and area loading still run.

An additional installed-build run pressed only Enter at tick 2,600 during
the movie. It took the original abort/cleanup path and reached active
gameplay. W at tick 4,000 changed the player's coordinates after that
transition, verifying keyboard movement after a skip. Space was first
supplied at tick 4,100, after the movie had already been skipped.

Music, sound effects and movie audio ran through the game's actual
SDRDRV/LIBSD/SPU2 path. Optional PCM captures from the earlier 233-second
audio run provided:

| Capture | Frames at 48 kHz | Peak | Clipped samples |
|---|---:|---:|---:|
| SPU2 source | 11,178,752 | 32,762 | 0 |
| Device callback | 11,173,920 | 32,762 | 0 |
| Windows process loopback | 10,405,920 | 22,524 | 0 |

The process loopback capture covered 216.790 seconds and 21,679 packets;
its capture tool reported **zero discontinuities**. Capture intervals differ
because the loopback process attached after startup. These checks establish
working sound output and bounded sample values, not exact PS2 hardware
sound or A/V synchronization.

## Relocated repository checks

The repository contains 20 [standalone source-only fixture packages](../tests/README.md),
plus the separate audio-loopback diagnostic. The original 16 were independently
configured, built and run from their new repository paths on native Windows:
**26/26 CTest entries passed**, with `/fp:strict`. The process audio-loopback
diagnostic also built. Existing production runtime libraries came from the
verified `RelWithDebInfo` build; recompiler libraries came from `Release`.
Fixtures use configurable paths rather than workspace defaults.

The complete 26-patch series was replayed with tools/full phases and repeat
application. After staging new files and Git line-ending normalization,
its tree exactly matches the tested source export:
`6c3edbccc9e71f1477863bd707e5758658e9b28b`. All 57 managed source blobs
also match the native build checkout after normalization.

The added GS depth package compiles the actual CPU backend and local-memory
code without retail assets. After relocation it passed 143,397 checks and
**2/2 CTest entries**, including an expected-failure control that restores
only the old ZTE behaviour. Focused upstream GS checks also passed **51/51**.
At the depth-fix stage, the full legacy GS suite had **41/72 passed**: all 31 failures also
occurred with the prior backend control. These results do not claim the
complete legacy suite is green or that all GS behaviour is correct.

The relocated GS alignment fixture compiles the real CPU backend and local-memory
code. It passed 853,552 checks with one worker and again with eight workers,
with zero changed feedback pixels or artificial boundary steps. A scratch
control restores only the prior sprite function: it fails 46,839 assertions
and reproduces 7,812 changed pixels and 217 extra 64-pixel boundary steps.
The source-only package has two default CTest entries, or three when that
optional expected-failure control path is supplied.

The relocated GS trace fixture exercises the actual trace implementation and
public types with synthetic CRT registers, transfers, sprites, environment
settings and an eight-byte snapshot callback. Its nine
fixed cases passed 59 assertions; six expected-failure controls restore the
prior behaviour. All 15 CTest entries passed. The old behaviour produces
14 failed assertions in 43 control checks. It also checks event APIs without
`onTick`, snapshot dumps after the trace interval, disabled/VRAM-only operation
and idempotent shutdown; it does not run a full guest or renderer. Existing
fixture results above were not rerun solely to add these three packages.

The relocated source-height fixture uses real `BeginTransfer`, `UploadImage`
and public `Present`, including the normal VRAM snapshot. Its independently
encoded gradient and bottom HUD marker check every visible pixel on CRT1,
CRT2 and both circuits, magnification, odd source heights, clamp order,
progressive/FIELD output and the initial small FRAME-field bob path. Nine
fixed cases passed 6,129,642 checks; six expected-failure controls restoring
only pre-height behaviour produced 4,072,731 failures in 4,081,608 checks.
All 15 CTest entries passed. The default-environment CRT2 case also passed
1,146,904 checks. These synthetic checks do not validate cross-frame weave
history or the host window's scaling.

The relocated `Test-KFIV.ps1` ran the installed executable to tick 900 in
16.283 seconds: normal code 0, no timeout/forced stop and a final frame.
It did not touch existing cards. The Test/Rebuild helpers were also checked
with controlled native executables under Windows PowerShell 5.1 and
PowerShell 7, including quoted paths, environment restoration, private
cards, timeout handling and artifact copying. New configuration/staging
helpers are intentionally separate from the compilation steps.

The original Linux workflow has been preserved. Shell syntax checks do not
constitute a Linux build or execution test of the new changes.

## Remaining limits

### Cave-door progression follow-up

The interactive native Windows Vulkan run reproduced a long floor fall
after the opening path's cave door. Both collision banks and all requested
map reads were complete before the fall. Patch 0029 corrects SQRT.S to read
Ft and RSQRT.S to divide Fs by sqrt(abs(Ft)); the actual generator fixture
passed 49 checks and nine old-code controls. The complete game was
regenerated with the same 28429/28161/268 function counts, 1352 fallback
warnings and zero errors; 124 generated files changed when staged by content.

With corrected collision calculations, the player crossed the map boundary
and then waited indefinitely for GS FINISH. The completed VIF1 frame had
4487 acyclic tags; its FINISH packet was in tag 4486, beyond the runtime's
4096-tag cutoff. Patch 0032 removes that cutoff using constant-space cycle
detection over the tag address and CALL return stack. Its synthetic fixture
passed 245 checks through actual memory, VIF, GIF and CPU GS CSR handling;
the old-limit control reproduced 59 failures. Both CTest entries passed.
VU execution is replaced by an MSCAL counter in this fixture.

The corrected native executable was tested interactively on the same route.
The user confirmed movement, camera control and inventory beyond the door,
then continued to the first NPC. The map selector changed from 0 to 1,
subsequent positions remained on successive floor heights, and the next
loader completed while gameplay continued. That exact tested executable
was installed for the existing desktop shortcut. Fresh tools/full repeat
application of all 32 patches reproduced source tree
`b035f6f9d78fa9259c414ad997d4bb84224913b2`, with all 64 managed source files
matching the native checkout. No Linux build was performed for these fixes.

This establishes progression past the reported doorway, not later-area or
full-playthrough stability. The newly reached first NPC initially had an
incorrect skeletal pose; the shared conversion follow-up below resolves it.
Returning to title and starting again could hang the intro; native replay
verification is ongoing.

### Shared skeletal conversion follow-up

The first NPC's character resource and animation payload matched the owned
disc; its world placement matrix was upright. The shared bone builder used
CVT.W.S for angle range reduction, but the runtime's host `nearbyintf`
rounded instead of performing the EE's truncation. Positive angles could
leave the principal interval and reverse the resulting polynomial rotations.
Two independent reconstructions reproduced all 21 corrupted local joint
matrices using the former conversion.

Patch 0034 corrects the shared runtime instruction with integer bit decoding,
truncation toward zero and sign saturation. It preserves FCR31 and does not
depend on the host rounding mode or perform undefined overflowing casts.
The actual decoder/translator compile-and-execute fixture passed 199,956
checks; restoring only the old macro produced 920 failures in 133,316 checks.
Both native Windows CTests passed. No character model, animation or physics
override was added; existing generated game code calls the corrected macro.

The full native runner was rebuilt with this header. Read-only RAM captured
beyond the door contains 21 joint matrices matching the independently
reconstructed truncating pose to 1.19e-7 maximum rotation error and 1.53e-5
maximum overall matrix error. Fresh application and repeat of all 34 patches
reproduce normalized tree `1ed68a278461aefe519657d06a8b51178b7f8c26`;
all 65 managed files match the native checkout and clean source export.
The user confirmed the first NPC is seated upright, with a matching
screenshot. That exact tested executable was installed for the existing
desktop shortcut. Conversation and later characters were not independently
verified. These checks do not establish accuracy of every EE operation or
a full playthrough.

The Vulkan text follow-up (patch 0028) replaces history weaving with a GPU
nearest bob of the current 224-row field. Title and inventory captures no
longer show displaced alternating text rows. Its native keyboard-input run
reached tick 4500 normally in 118.064 seconds, including inventory and
pause/resume. The relocated fixture passed 11/11 CTests, including two
old-source controls; the prior adaptive path failed 1,290,243 pixel checks.
Fresh 28-patch application and repeat reproduce the exported source tree.

- Opening pictures and audio work. The CPU reference retains its older
  weave path; the Vulkan field fix was checked on the title, opening and
  inventory. Other video paths remain unverified.
- The repeated 64-pixel gameplay strip corruption is fixed. Broader 3D
  rendering accuracy remains unverified and gameplay runs below full speed.
  The old Linux first-area measurements in `NOTES.md` are historical measurements,
  not a benchmark of this Windows build.
- SPU2 reverb uses an approximate average/hold path rather than the
  hardware half-band filter; SPDIF and 32-bit AutoDMA are unimplemented.
  IPU accounting does not model the complete hardware bit parser or REFS
  bus interaction.
- A full playthrough, later areas, saving/loading at a real save point and
  Windows controller integration have not been verified.

The [Windows guide](windows.md) describes how to reproduce the build from
an owned disc. The integration helpers were smoke checked and the fixtures
were rebuilt at their repository paths; a separate full clean retail
generation/build was not repeated solely to validate the documentation.

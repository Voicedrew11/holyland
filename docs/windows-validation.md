# Native Windows validation (2026-10-09)

The USA boot ELF `SLUS_203.18` was statically recompiled and run as a native
x64 executable on Windows 11 with MSVC 19.44. The IOP and VU1 remain
interpreters in the supporting runtime; GS rendering is software based.
The verified build was `RelWithDebInfo`, `/O2 /Ob1 /DNDEBUG /fp:strict`,
with FFmpeg enabled and matching shared runtime DLLs.

## Generation and source fixes

The final generation processed 28,429 functions, recompiled 28,161 and used
268 SDK stubs. It reported 1,352 JR/JALR fallback warnings, zero errors,
zero decode failures, zero skipped functions and zero unhandled
instructions. The diagnostics counted 119,442 registered fallback entries.
The older 999-warning report was a historical
baseline, before the final residual-entry regeneration.

The added series changes 41 source/license/build files across Windows,
controls, IOP file completion, audio, clocks, EE scheduling/callbacks,
MPEG/IPU and the generator. Audio patch 0022 accounts for 31 of those files.
The focused generator change is proposed upstream
in [PS2Recomp PR #279](https://github.com/ran-j/PS2Recomp/pull/279).
Patch `0022` remains necessary for the pinned `c5a9d02` base until that pin
moves to a version containing the fix. Rebuilding the tools and regenerating
`register_functions.cpp` is required.

## Retail integration run

A combined run reached tick 10,500 and exited normally with code 0 in
233.323 seconds, without timeout or forced termination and with private
test cards. Title/New Game/brightness selection entered the full opening
audio path. The MPEG stream accepted all 2,772 retail pictures, but the
presented opening image remained black.

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
original first-area intro add time before free movement. The black opening
picture can therefore look like a hang, especially with PC audio muted.
Enter sends the game's original Start input to skip the movie after it
begins; normal movie/audio cleanup and area loading still run.

An additional installed-build run pressed only Enter at tick 2,600 during
the movie. It took the original abort/cleanup path and reached active
gameplay. W at tick 4,000 changed the player's coordinates after that
transition, verifying keyboard movement after a skip. Space was first
supplied at tick 4,100, after the movie had already been skipped.

Music, sound effects and movie audio ran through the game's actual
SDRDRV/LIBSD/SPU2 path. Optional PCM captures provided:

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

All 16 [source-only packages](../tests/README.md) were independently
configured, built and run from their new repository paths on native Windows:
**26/26 CTest entries passed**, with `/fp:strict`. The process audio-loopback
diagnostic also built. Existing production runtime libraries came from the
verified `RelWithDebInfo` build; recompiler libraries came from `Release`.
Fixtures use configurable paths rather than workspace defaults.

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

- Opening movie image remains black, although its audio and natural return
  work. Other video paths remain unverified.
- 3D graphics are partly wrong and gameplay runs below full speed. The old
  Linux first-area measurements in `NOTES.md` are historical measurements,
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

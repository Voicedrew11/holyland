# Optional MSVC VU profile optimization

This workflow relinks an already built native Windows runner with MSVC
profile guided optimization (PGO). It keeps the original runner intact and
copies the exact object/archive inputs into a separate private directory.
Only units already compiled with `/GL` can be profiled. The performance
runtime uses `/GL` on the three VU units and keeps their `/fp:strict`
arithmetic. This workflow does not enable fast math or recompile the
generated EE game files.

Use Python 3.11 or newer and the same x64 MSVC/Windows SDK that built the
runner. Start in its x64 Visual Studio developer PowerShell so `LIB`
identifies the matching libraries. Build the complete current runtime
first; changing sources or generated catalogs later requires a new PGO
snapshot and training run. Follow [the Windows build guide](windows.md)
before these optional steps.

## Freeze and link private variants

Choose an unused output directory outside this repository, the runtime
source checkout and its build tree. The runner build path below is the
directory containing `ps2EntryRunner.dir` and `RelWithDebInfo`, usually
`<runtime-build>/ps2xRuntime`. Replace all example paths/tool versions.

```powershell
$pgo = 'C:\KFIV-private\pgo-final'
$helper = 'scripts/windows/Prepare-KFIVPgo.py'
python $helper prepare `
  --runner-build-dir 'C:\KFIV-dev\runtime-build\ps2xRuntime' `
  --source-dir 'C:\KFIV-dev\PS2Recomp' `
  --msvc-bin 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64' `
  --sdk-bin 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64' `
  --output $pgo
if ($LASTEXITCODE) { throw 'PGO snapshot failed.' }
python $helper link-baseline --output $pgo
if ($LASTEXITCODE) { throw 'Baseline relink failed.' }
python $helper link-training --output $pgo
if ($LASTEXITCODE) { throw 'Instrumented relink failed.' }
```

Preparation copies the original EXE/PDB, explicit link inputs and adjacent
runtime DLLs, and records SHA256 hashes for the inputs, tools and response
files. `--runtime-dll-dir` can supply the deployed runner's DLL directory
when it differs from the build output. The training directory additionally
receives the matching `pgort140.dll`. Each relink checks the frozen hashes
and writes its outputs/logs under the private PGO directory. The script
never launches the game or changes the ordinary build.
The helper accepts the ordinary KFIV runner link graph. It rejects optional
file-bearing LINK features such as `/DEF`, `/ORDER`, `/WHOLEARCHIVE` and
additional metadata/profile output paths rather than leaving live inputs
or outputs outside that snapshot.

## Train, optimize and verify

Launch `<pgo>/train/ps2EntryRunner.exe` through your usual runner test or
launch configuration, using your own extracted disc and private memory
cards. Keep `PS2X_VU1_LIFT=0`; this workflow evaluates the bounded native
catalog/interpreter, rather than the separate upstream static lift.
Exercise representative gameplay and quit normally. An instrumented
build is slower and should not be used to measure the final speed.
Normal exit flushes `.pgc` files beside the training EXE.

Then merge those files and create the optimized variant:

```powershell
python $helper optimize --output $pgo
if ($LASTEXITCODE) { throw 'Profile merge or optimized relink failed.' }
```

Optimization leaves the original `training.pgd` unchanged and merges into
a disposable copy under `merge-attempts/<number>/`. It records the
instrumented EXE, individual PGC hashes and PGD hashes before/after the
merge in `profile-inputs.json`. Runtime DLLs,
profiling runtime, link inputs/tools/responses and training identity are
checked again before and after linking. `/USEPROFILE` receives an exclusive
writable `profile-work.pgd` copy because LINK can update PGD bookkeeping.
The unmerged training PGD, merged attempt and `profile-input.pgd` remain
pinned. The final
`optimized-result.json` records the input/output PGD, response, log,
EXE/PDB and preserved-file hashes. Only that recorded executable is the
completed optimized result.

A helper stage holds an exclusive `.pgo-stage.lock`; if its process crashes,
inspect that stage before removing the stale marker. A failed merge keeps
its partial PGD and log in that attempt directory. After quitting training
and resolving the reported failure, retrying `optimize` starts from the
unchanged training PGD in a new attempt, preserving the failed output.
Once `profile-inputs.json` records a completed merge, the helper refuses
another merge. If linking subsequently fails, retain the evidence and use
a fresh snapshot. Older unrecorded merge attempts are also rejected because
their training PGD may already have been modified.

For a snapshot made with an older helper that completed `/USEPROFILE` but
rejected LINK's PGD
bookkeeping update, preserve its outputs and use the explicit recovery stage:

```powershell
# Review and retain the current training.pgd SHA256 first.
$expectedPgd = (Get-FileHash -LiteralPath "$pgo\training.pgd" -Algorithm SHA256).Hash
python $helper recover-optimized --output $pgo --expected-pgd-sha256 $expectedPgd
if ($LASTEXITCODE) { throw 'Isolated recovery relink failed.' }
```

Recovery requires the successful prior link log, matching training identity
and unchanged PGCs. It links into a fresh `optimized-recovered` directory
using an isolated copy of the explicitly pinned current PGD, without merging
again or changing any previous output. It records that recovery provenance
and fails if the preserved inputs change during the new link. A failed
recovery directory is evidence; inspect it rather than overwriting it.

Use the baseline and optimized runners with matching arguments, graphics,
scene and test duration. Check actual game update cadence and attack/input
timing as well as presentation FPS. Verify registers, floating-point bits,
flags, cycles, memory and PATH1 output against the same reference with
the [VU verifier](../tests/vu-performance-tests/README.md) and
[authored native catalog fixture](../tests/vu-native-tests/README.md)
before accepting a compiler/profile change. Their ordinary builds do not
prove an arbitrary profiled runner; use the same source/catalog/compiler
inputs and run an independent optimized differential fixture as well.

Keep generated game code, profiles, frozen objects/libraries, catalogs,
logs and binaries outside Git. This repository supplies only the workflow
source. No Linux PGO or full-playthrough validation is implied.

The [helper guard tests](../tests/pgo-helper-tests/README.md) use mocked
merge/link operations and authored inputs to check failure and retry
semantics without MSVC or game data.

MSVC's [PGO overview](https://learn.microsoft.com/en-us/cpp/build/profile-guided-optimizations?view=msvc-170)
describes the `/GL`, `/GENPROFILE` and `/USEPROFILE` stages. The helper
merges the explicit training-directory PGC files with `pgomgr` before
`/USEPROFILE`; a subsequent `PG0188` message about no PGC beside the PGD
is compatible with that explicit merge. A mismatched-profile `PG1052`
message fails the workflow. Never mix profiles from different instrumented
link identities, even if their source appears identical.

# Building and running on native Windows

Use your own USA disc image (`SLUS-20318`). Ghidra is unnecessary because
the function map is committed. Keep disc files, generated code and builds
outside this repository. These manual steps use separate helpers for each
stage; they do not install compilers or obtain game data.

## Prerequisites and paths

Use Windows 11 x64, Visual Studio 2022 or Build Tools with Desktop development
with C++ and a Windows SDK, Git, Python 3.8+, CMake 3.21+, and several GB of
free space. Validation used MSVC 19.44. Network access is needed for the
build dependencies, including the pinned shared FFmpeg SDK.

Open PowerShell in this repository and choose external paths. Replace the
example CMake path with your installed CMake if needed.

```powershell
$holyland = (Resolve-Path .).Path
$checkout = 'C:\KFIV-dev\PS2Recomp'
$tools = 'C:\KFIV-dev\tools-build'
$runtime = 'C:\KFIV-dev\runtime-build'
$game = 'C:\KFIV-game'
$iso = "C:\Games\King's Field The Ancient City.iso"
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
```

## Apply the pinned series and build tools

Create a **new** private checkout. The helper rejects an existing checkout
at another commit without resetting edits. Applying the full series also
applies the generator fix needed by the tools.

```powershell
git clone --recurse-submodules https://github.com/ran-j/PS2Recomp.git $checkout
git -C $checkout checkout --detach c5a9d02
git -C $checkout submodule update --init --recursive
& "$holyland\scripts\windows\Apply-KFIVPatches.ps1" -CheckoutPath $checkout
& $cmake -S $checkout -B $tools -G 'Visual Studio 17 2022' -A x64 `
  -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF
& $cmake --build $tools --config Release --target ps2_recomp --parallel 8
```

For a separate tool checkout, `-ToolsOnly` filters patches to `ps2xRecomp/*`;
the full phase can follow later. Completed phases are recorded in Git's
private `kfiv-patches` directory with the pin, patch hashes and affected
file hashes. Repeating an unchanged phase verifies and skips it. Changing
the pin, patches or managed files requires inspection and a fresh checkout.
A partial failure preserves local edits but may leave earlier patches
applied. A manually patched full series without a matching record may also
require a fresh checkout. Neither helper resets files or changes the real index.

## Extract, verify and regenerate

```powershell
python "$holyland\scripts\extract_elf.py" --all $iso $game
python "$holyland\scripts\windows\Prepare-KFIVSources.py" config --game-dir $game
& "$tools\ps2xRecomp\Release\ps2_recomp.exe" "$game\config.toml"
```

The configuration helper verifies the boot ELF's SHA256 and changes only
the three path settings from `kfiv/config.toml`. Check the generator summary:
the verified US disc processed 28,429 functions, recompiled 28,161 and used
268 stubs, with **1,352 warnings / 0 errors**. The warnings are JR/JALR
fallback promotions. Rebuild the patched recompiler and regenerate all
sources after generator changes; an older `register_functions.cpp` does
not adopt patch `0022`.

Stage the private output by content. The generated function table replaces
the runtime's tracked placeholder. Keep this checkout private and never
stage generated game code for a contribution. The skip-worktree flag can
later be cleared with `git update-index --no-skip-worktree`.

```powershell
git -C $checkout update-index --skip-worktree ps2xRuntime/src/runner/register_functions.cpp
python "$holyland\scripts\windows\Prepare-KFIVSources.py" stage --game-dir $game --checkout $checkout
```

Staging copies changed `.h` and `.cpp` files without deleting files. Use a
fresh runtime checkout when changing to output from a different game or
generation layout, so obsolete generated files cannot enter the build.

## Build and stage the runtime

The tested native x64 configuration is `RelWithDebInfo`, `/O2 /Ob1 /DNDEBUG
/fp:strict`. The Windows patch adds `/bigobj`, parallel compilation and a
16 MiB runner stack. Strict floating point preserves the runtime's VU/GS
rounding-mode changes.

```powershell
& $cmake -S $checkout -B $runtime -G 'Visual Studio 17 2022' -A x64 `
  '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG /fp:strict' `
  -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_RECOMP=OFF `
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_IOP_BUILD_TESTS=OFF `
  -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_FFMPEG=ON `
  -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_RUNTIME_LOGS=ON
& "$holyland\scripts\windows\Rebuild-KFIV.ps1" `
  -BuildDirectory $runtime -GameDirectory $game -CMakePath $cmake
```

The runtime stages matching FFmpeg DLLs beside the built executable; the
rebuild helper copies these and the executable into `$game`. It does not
regenerate game code or touch saves. Validation used the
`System233/ffmpeg-msvc-prebuilt` shared LGPL package `n7.1-241205`.
Preserve its license directory when packaging DLLs, along with the adapted
SPU's MIT notice. For a local copy:

```powershell
$ffmpeg = Join-Path $runtime 'ThirdParty\ffmpeg-prefix\src\ffmpeg_external'
Copy-Item -LiteralPath "$ffmpeg\licenses" -Destination "$game\FFmpeg-licenses" -Recurse -Force
Copy-Item -LiteralPath "$checkout\ps2xIOP\licenses\RecompOne-SPU.txt" -Destination $game -Force
Copy-Item -LiteralPath "$checkout\ps2xRuntime\licenses\Verdite2-mouse-indicator-LICENSE" -Destination $game -Force
```

## Launch and controls

```powershell
& "$holyland\scripts\windows\Run-KFIV.ps1" -GameDirectory $game
```

To create a desktop shortcut, keep the working directory set to the game
directory. This example refuses to replace an existing shortcut:

```powershell
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Desktop')) "King's Field IV Native.lnk"
if (Test-Path -LiteralPath $shortcutPath) { throw "Shortcut already exists: $shortcutPath" }
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = Join-Path $game 'ps2EntryRunner.exe'
$shortcut.Arguments = '"' + (Join-Path $game 'SLUS_203.18') + '"'
$shortcut.WorkingDirectory = $game
$shortcut.IconLocation = "$game\ps2EntryRunner.exe,0"
$shortcut.Save()
```

The game reads loose disc files from its working directory and stores cards
in `mc0` and `mc1`. After accepting brightness with F, the opening movie
plays audio for about 92 seconds while its picture remains black in this
development build. Press Enter after the movie starts to skip it through
the game's original Start input, or let it finish naturally. Loading and
the first-area fade can take additional time.

| Input | Action |
|---|---|
| W/S, A/D | Forward/back, strafe |
| Mouse | Look after clicking to capture in gameplay |
| Arrow keys | Turn/move; select in menus |
| Space / left mouse | Attack |
| F / middle mouse | Use, examine, talk, confirm; hold to run |
| Q / right mouse | Magic |
| Tab / Escape | Inventory/menu or back; release mouse |
| Enter | Start/pause |
| Right Shift | Select/accessory |
| Menu arrows / wheel | Select item |
| Menu F / left mouse; Tab / Escape / right mouse | Confirm; back |

Mouse sensitivity defaults to 0.15 degrees/pixel with normal vertical
direction. The capture click is consumed. Closing a menu or pause restores
the previous capture state. Focus loss releases capture; after focus returns,
click again to capture. The brief top-right Verdite glyph is solid
while captured and diagonally cut while released. Windows controller
integration has not been checked.

## Hidden verification runs

Use a new external run directory for an isolated run with new cards:

```powershell
& "$holyland\scripts\windows\Test-KFIV.ps1" `
  -Executable "$game\ps2EntryRunner.exe" -GameDirectory $game `
  -RunDirectory 'C:\KFIV-dev\runs\intro-first-area' -ExitTick 10500 `
  -InputScript '1100:START:10,1500:CROSS:10,1900:CROSS:10' `
  -DumpEvery 150 -Stats -Lockstep
```

The helper junctions `DATA`, copies the small boot/IOP files, creates private
cards and retains stdout/stderr, PNGs and `result.json`. `-CopyMemoryCards`
copies existing cards into the test; originals are never used. Managed
environment variables are restored afterwards. The default directory is
under `%TEMP%\KFIV-tests`; run directories must be new or empty.

`-Environment` accepts optional runtime variables, including
`PS2X_HOST_INPUT`, `PS2X_INPUT_TRACE`, `PS2X_AUDIO_DUMP`,
`PS2X_AUDIO_SOURCE_DUMP` and `PS2X_AUDIO_TRACE`. Use different paths for
device and source WAV captures. Analyze a capture with:

```powershell
python "$holyland\scripts\windows\Analyze-KFIVAudio.py" capture.wav --json summary.json
```

See [validation and remaining limits](windows-validation.md) and
[source-only test instructions](../tests/README.md). A separate full clean
retail generation/build was not repeated solely to validate this guide;
the relocated helpers and fixtures were checked against the verified build.

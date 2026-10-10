# Pinned Windows Vulkan GS dependency

`Prepare-ParallelGS.ps1` retrieves source with GitHub CLI, verifies the pins
below, and applies this package's small-transfer correction. It initializes
only Granite, volk and Vulkan-Headers. It does not download a Vulkan SDK,
shader compiler, drivers, retail data or prebuilt renderer binaries.

| Source | Commit | Path relative to paraLLEl-GS |
| --- | --- | --- |
| [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) | `cc6184af7e0c03da603045ca371ffa5dae9b0655` | `.` |
| [Granite](https://github.com/Themaister/Granite) | `16e7395f6a4858c1783dbf6f521f90b9d5f82ac5` | `Granite` |
| [volk](https://github.com/zeux/volk) | `47cddf7ed97b94118a08aacb548a411188e016cc` | `Granite/third_party/volk` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `11d6898377797e07dbd543aaaa367e4465074597` | `Granite/third_party/khronos/vulkan-headers` |

The nested commits are verified against their parent's committed gitlinks.
Fresh destinations use `gh repo clone --filter=blob:none --no-checkout`, then
check out exactly the selected commit. No recursive submodule download is
needed. The resulting nested repositories occupy the expected submodule paths;
`git submodule init` registers only these three paths in local Git configuration.
The helper does not change `.gitmodules` or the parent's index.

The required patch is `parallel-gs-small-transfers.patch`, SHA-256
`D651CA44EEB7370AB7402D17255399D2C9F37A8CAA80A662FC41F63A559693A6`.
It changes only `gs/gs_interface.cpp`: rounds upload word counts and readback
word counts upward and zeroes allocated readback padding. The affected
source remains under its upstream LGPL-3.0-or-later license.

Use Git, GitHub CLI with working GitHub access, Visual Studio 2022 C++ Build
Tools with a Windows SDK, and CMake 3.21 or newer. The optional dependency-only
build uses the Visual Studio 2022 x64 generator. A compatible installed Vulkan
GPU driver is required to execute the tests, but not to compile the library.
The pinned project contains generated SPIR-V in `gs/shaders/slangmosh.hpp`;
standalone mode disables runtime shader compilation and optional components.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Prepare-ParallelGS.ps1 `
  -SourceDirectory 'C:\src\parallel-gs'

# Optional: build only the static dependency; this does not execute GPU work.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Prepare-ParallelGS.ps1 `
  -SourceDirectory 'C:\src\parallel-gs' -Build `
  -BuildDirectory 'C:\build\parallel-gs' -Configuration Release `
  -CMakePath 'C:\path\to\cmake.exe'
```

`-ExecutionPolicy Bypass` applies to that PowerShell process; it does not change
the machine policy. Quote paths containing spaces. All destinations are
explicit parameters, with no developer workspace paths. The helper is written
for native Windows PowerShell 5.1 as well as PowerShell 7.

The DLL CRT setting is
`MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` (`/MD` or Debug `/MDd`), matching the
native runner. paraLLEl-GS and Granite retain their upstream floating-point
compiler options. This package's fixture targets separately use `/fp:strict`.
No compiler flags or existing source files are silently rewritten.

On rerun, matching checkouts and an already applied patch are retained. An
existing checkout at a different HEAD, a nonempty non-repository destination,
a changed patch hash, or a patch conflict fails with an explanation. Existing
edits and untracked files are preserved. The helper never resets, cleans,
deletes, or switches an existing checkout. Missing dependencies retrieved
before a later failure remain available for inspection. Dirty source is allowed
and reported; it can affect the resulting build and is not an exact-source
reproduction claim.

Build the fixtures using both explicit source paths, as shown in
[README.md](README.md). Fixture CTest execution uses the GPU; run it separately
from game or renderer timing measurements. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for retained licenses and source
redistribution contents.

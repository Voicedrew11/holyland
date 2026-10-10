# GS scanout-height regression

This GPL-3.0-only, source-only fixture exercises the actual GS backend through
`BeginTransfer`, `UploadImage`, and public `Present`. The latter reaches
`PresentFromLocalMemory` after its normal VRAM snapshot. It uploads an independently
encoded row/column gradient, with a 32-row HUD marker at source rows 416..447.
Every visible pixel and its alpha are checked; no game assets, generated game
code, saved frames, binaries, or prior renderer sources are included.

The captured KFIV gameplay state uses CRT2, CT32, FBW10, DW2559/MAGH3,
DH895/MAGV0, and SMODE2 INT1/FFMD1. Its timing describes 640×896 before
the FRAME-mode division and a 640×448 source image. Applying the 512-row host
limit before dividing incorrectly reads only 256 source rows and presents a
512-row image with duplicated rows. The regression requires all 448 source rows,
including the complete HUD and the rightmost column, at their original proportions.

The expected ordering follows the FRAME-mode source-height calculation in
[PCSX2 SetRects at revision 35560895](https://github.com/PCSX2/pcsx2/blob/355608952714678b3c57832fb82dc6a42956ed25/pcsx2/GS/GSState.cpp#L7306):
decode magnification, then ceil-divide interlaced FRAME height by two, then apply
the host limit. This package targets Holyland's patched runtime, whose existing
small FRAME-field weave remains enabled. Current upstream main has a different
presentation implementation; this is not an assertion that the same production
patch can be copied there unchanged.

| Case | Boundary checked |
| --- | --- |
| `crt2_game` | Exact captured timing at FBP0 and FBP140, both field parities; 640×448 and every row |
| `crt1_game` | Same timing on independently enabled CRT1 |
| `dual_crt` | Both circuits enabled, 448/449 source heights, and constant-alpha selection of each source |
| `magnification` | MAGH3/7 and MAGV1; 640/512 widths and full 448-row source |
| `odd_rounding` | 897 decoded timing rows produce 449 source rows, also after MAGV1 |
| `frame_clamp` | 512 source rows and heights above that limit, with source division before clamping |
| `progressive` | INT0 does not halve, including FFMD1; magnification and direct width/height clamping |
| `field_rows` | INT1/FFMD0 preserves full height and selects the requested source parity |
| `small_frame` | A single initial FRAME presentation preserves the existing 224-to448 bob behavior |

Each case runs in a separate process. The small-frame case deliberately checks
only the first presentation; it does not test cross-frame weave history, relative
CRT placement, or the host's texture upload and window scaling. The framebuffer
upload and scanout use real backend code, while timing requests, pixel patterns,
and expected dimensions are synthetic. The output buffer has the backend's fixed
640-pixel row stride even when its logical width is 512.

From the repository root, configure a build outside the source tree:

```powershell
$Ps2Recomp = 'C:\src\PS2Recomp' # Patched runtime checkout, supplied by you.
$Build = 'C:\build\gs-scanout-height'
$CMake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$CTest = Join-Path (Split-Path $CMake) 'ctest.exe'
& $CMake -S tests/gs-scanout-height-tests -B $Build -G 'Visual Studio 17 2022' -A x64 "-DPS2RECOMP_DIR=$Ps2Recomp"
& $CMake --build $Build --config RelWithDebInfo --parallel 2
& $CTest --test-dir $Build -C RelWithDebInfo --output-on-failure
```

`PS2RECOMP_DIR` is required and has no workspace default. CMake selects the
checkout's real GS memory and trace sources; the trace source is optional for
checkouts that do not have it. MSVC uses `/fp:strict` with the configuration's
RelWithDebInfo optimization flags. CTest explicitly enables normal weave behavior
(`PS2X_NO_WEAVE=0`) and one raster worker for reproducibility.

For an optional negative control, supply an external copy of the backend from
immediately before the height-order fix with
`-DGS_SCANOUT_PRIOR_SOURCE=C:\controls\gs_cpu_backend_before_height.cpp`.
Six regression cases then run against that source. Their nonzero exits are
expected and marked `WILL_FAIL`; unchanged progressive/FIELD/small-frame behavior
is checked by the normal cases. The old backend is not distributed here.

Native Windows validation used MSVC 19.44.35228, Windows SDK 10.0.26100, and
RelWithDebInfo. The nine fixed cases passed 6,129,642 checks with zero failures.
The six pre-fix controls performed 4,081,608 checks and produced 4,072,731
failures, so CTest passed 15/15 entries including those expected failures. The
captured CRT2 case also passed 1,146,904 checks with `PS2X_NO_WEAVE` and
`PS2X_GS_THREADS` unset, confirming the default presentation behavior.
Linux, a full runtime build, and retail game behavior are not validated by this
standalone package.

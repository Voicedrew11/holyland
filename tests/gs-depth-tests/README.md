# GS disabled-depth regression

These tests compile the real CPU GS backend, local-memory code and trace code.
They need no game data, generated guest code, FFmpeg or existing runtime build.

```powershell
cmake -S tests/gs-depth-tests -B C:/KFIV-dev/test-builds/gs-depth-tests -G "Visual Studio 17 2022" -A x64 `
  -DPS2RECOMP_DIR=C:/path/to/patched/PS2Recomp
cmake --build C:/KFIV-dev/test-builds/gs-depth-tests --config RelWithDebInfo --parallel 4
ctest --test-dir C:/KFIV-dev/test-builds/gs-depth-tests -C RelWithDebInfo --output-on-failure -V
```

The fixed backend must pass. The prior-behavior control removes only the two
production hunks in a scratch copy and must return a failure exit code. CTest
uses `WILL_FAIL` for that control; inspect its output to see the disabled-test
and movie-color failures.

Coverage:

- All four `TEST.ZTST` values while `ZTE=0`: framebuffer writes pass and the
  depth buffer remains unchanged, even when `ZBUF.ZMSK=0`.
- Enabled NEVER, ALWAYS, GEQUAL and GREATER comparisons, including equality
  boundaries and depth-write masking.
- A synthetic 640x448 CT32 upload and the opening movie's captured DECAL
  sprite settings: `TEST=0`, zero vertex RGB, destination 640x224 and an
  inherited depth base that aliases the movie texture. Every destination
  pixel becomes visible, and disabled depth writes preserve the texture.
- CRT2-only scanout with `PMODE.ALP=0` still presents the movie color.

The synthetic sprite checks visible RGB, not an exact filtering result at its
texture edge. The package does not claim to test the full decoder, movie
timing, interlace weaving, native audio or all gameplay rendering.

Depth behavior is consistent with the primary
[PCSX2 software GS implementation](https://github.com/PCSX2/pcsx2/blob/355608952714678b3c57832fb82dc6a42956ed25/pcsx2/GS/Renderers/SW/GSRendererSW.cpp):
disabled `ZTE` masks depth writes; comparisons require `ZTE`; the disabled
comparison selector becomes ALWAYS.

Verified on native Windows with MSVC 19.44, x64 RelWithDebInfo and `/fp:strict`.
The fixed target passed 143,397 checks. The controlled prior target returned
143,369 failures, including the black movie sprite. Both CTest entries passed.
Other platforms have not been run.

This package is licensed under GPL-3.0-only. It compiles code from PS2Recomp,
whose copyright notices and GPL license apply to the linked test executables.
The included tests and build files use the same license. See `LICENSE`.

# GS sprite alignment regression

This source-only fixture compiles the actual CPU GS backend, local-memory
implementation and trace code. It needs no game assets, generated guest code,
FFmpeg or existing runtime build. It tests sprite rasterization and texture
alignment; it does not change presentation or apply image filters.

```powershell
cmake -S tests/gs-alignment-tests -B C:/KFIV-dev/test-builds/gs-alignment -G "Visual Studio 17 2022" -A x64 `
  -DPS2RECOMP_DIR=C:/path/to/patched/PS2Recomp
cmake --build C:/KFIV-dev/test-builds/gs-alignment --config RelWithDebInfo --parallel 4
ctest --test-dir C:/KFIV-dev/test-builds/gs-alignment -C RelWithDebInfo --output-on-failure -V
```

The two default CTest entries run the same assertions with one and eight GS
workers. An optional `GS_ALIGNMENT_PRIOR_SOURCE` CMake path adds a prior-backend
control executable that must fail the assertions; CTest marks that entry
`WILL_FAIL`. The tested local control replaces only `DrawSprite` in an otherwise
identical backend source with the pre-fix implementation. The control source is
external to this package; no retail content or old runtime implementation is
bundled here.

Coverage includes:

- Independent scalar texture interpolation with all 16 values of the UV
  fractional nibble, using nearest and bilinear texture filtering.
- Reversed X, reversed Y and both reversed axes: each coordinate endpoint keeps
  its corresponding texture coordinate. The second vertex remains the source
  of the sprite's flat color and Q.
- Fractional `XYOFFSET`, integer GS sample positions, ceil-exclusive bounds,
  63.5-pixel endpoints, clipped interpolation, zero-size sprites and untextured
  bounds.
- Four adjacent 64-pixel CT32 framebuffer feedback strips, reversed on both
  axes, with a fractional identity texture mapping. They share the real
  single-page texture cache, have no intervening `TEXFLUSH`, and take the
  backend's serial-feedback draw path. Every pixel must remain unchanged, and
  strip/page boundaries must retain the original one-unit gradient.

The feedback input is deliberately an identity mapping: because correct draws
write the same pixels they sample, its expected result does not depend on
choosing a new texture-cache invalidation policy. The regression reproduces
column seams caused by sprite endpoint handling without asserting that every
possible self-feedback effect should use a full-frame snapshot. Bilinear
oracle checks permit one color unit for floating-point operation-order
rounding; feedback, coverage and untouched-pixel checks are exact.

The coordinate rules follow primary implementations in pinned PCSX2 source:
[vertex conversion](https://github.com/PCSX2/pcsx2/blob/355608952714678b3c57832fb82dc6a42956ed25/pcsx2/GS/Renderers/SW/GSRendererSW.cpp#L219)
preserves fractional XYOFFSET/UV and uses the second vertex's Q for sprites;
[sprite rasterization](https://github.com/PCSX2/pcsx2/blob/355608952714678b3c57832fb82dc6a42956ed25/pcsx2/GS/Renderers/SW/GSRasterizer.cpp#L1088)
sorts matching texture components per axis, uses ceil-exclusive bounds, and
interpolates at integer pixel positions. Linear sampling retains its separate
half-texel offset.

Native Windows validation: MSVC 19.44, x64 RelWithDebInfo, `/fp:strict`.
Each fixed run passed 853,552 checks with zero failures, zero changed feedback
pixels and zero artificial boundary steps. The prior-sprite control returned
46,839 failures, including 7,812 changed feedback pixels and 217 artificial
boundary steps. CTest passed 3/3 entries with that expected-failure control.
Other platforms have not been run. These results do not assert that every
gameplay renderer or presentation issue is resolved.

This package is GPL-3.0-only. PS2Recomp's copyright notices and GPL license
apply to the linked implementation. See `LICENSE`.

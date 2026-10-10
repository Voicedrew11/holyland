# Fixed game updates and independent presentation

These source-authored fixtures contain no game assets or translated retail
instructions. Build outside the repository with `PS2RECOMP_DIR` pointing at the
patched runtime. `RUNTIME_BUILD_DIR` selects an already-built native runtime;
without it, only the pure pacer test is built.

```sh
cmake -S tests/fixed-frame-tests -B /private/checks/fixed-frame \
  -DPS2RECOMP_DIR=/private/PS2Recomp -DRUNTIME_BUILD_DIR=/private/runtime-build
cmake --build /private/checks/fixed-frame
ctest --test-dir /private/checks/fixed-frame --output-on-failure
```

On Windows use the Visual Studio x64 generator and `--config Release` / `-C Release`.
The runtime library defaults to `RelWithDebInfo`; `RUNTIME_CONFIG` overrides it.
Linux links its own static libraries and pkg-config FFmpeg, never Windows objects.

- The pacer verifies exact rational 30000/1001 Hz deadlines over 1,001 seconds,
  short-overrun recovery, long-stall recovery and independent instances.
- The runtime fixture links the actual scheduler, memory and game-override
  registry. Authored functions replace only the generated game dispatch. It
  checks metadata exclusion, saved registers, original timer invocation,
  repeated registration, teardown, elapsed update cadence, the independent
  physical field clock and continued guest callback service. Delayed guest
  callbacks can coalesce; they are not the physical field counter.
- The renderer fixture uses the actual CPU GS. An authored textured triangle
  moves between updates. Its deterministic halfway image must differ from
  both original frames, the endpoint must match, repeated replays must be
  identical, guest VRAM and original scanout must remain unchanged, and reset
  must discard old frames. Non-game suspension and a missing-boundary timeout
  must show live presentation; returning to gameplay must checkpoint fresh
  memory without matching geometry from before the menu/scene transition.

These are focused mechanism checks, not a retail playthrough or proof that
all objects can be matched across frames. VU exactness uses the separate
native/stepping packages. Retail renderer verification additionally compares
every completed frame and all 4 MiB of VRAM with the actual game backend via
`PS2X_INTERPOLATION_VERIFY=1`; that mode deliberately adds readback overhead.

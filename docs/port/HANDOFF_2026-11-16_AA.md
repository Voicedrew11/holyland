# Handoff 2026-11-16: SMAA, and the groundwork for SMAA T2x and DLAA / FSR 4

One session's work, for the next session. Two parts, in this order:

- **A. SMAA 1x**: a third anti-aliasing option (Off / FXAA / SMAA). Finish it whole: built, measured, pictured, in
  the overlay.
- **B. The groundwork for temporal anti-aliasing**: camera jitter, motion vectors, the shown picture's depth, the
  history, and the interface that hands them to a method. Proven by debug views, not by a method yet.
- **C. (only if B is proven in the same session) SMAA T2x** as the first method on top of B. Otherwise it is the next
  session's first job.

Read `docs/port/PC_RENDERER.md` "Anti-aliasing (2026-11-16)" first: what is there now and why.

## Status (end of the 2026-11-16 session; branch feat/remove-msaa-fxaa, not committed)

- **A. SMAA 1x: done.** Off byte for byte main's; pictured at 009 and 063, 2x and 4x, against FXAA; ASan run clean;
  0.17 ms a picture at the owner's settings (FXAA 0.10).
- **B. Groundwork: built, proven for the static world** (depth, camera motion against a measured shift,
  reprojection, jitter on the 3D only). Not done: per-vertex motion for moving models, a vblank's own picture's depth,
  a reactive mask. PC_RENDERER.md, "Temporal inputs".
- **C. SMAA T2x: built** (`--antialias smaa_t2x`), pictured turning at 009 and 063 against SMAA 1x, ASan clean,
  0.47 ms a picture. Not checked: a fight, under water. The owner has not seen it yet.
- Runs: the owner said (2026-11-16) not to ask before them; run one at a time, spaced.

## 0. The owner's position (do not relitigate)

- **MSAA is gone for good** (2026-11-16, "a ball and chain on this project"). Do not propose it again, nor SMAA S2x
  (it needs MSAA).
- **"TAA sucks"**: blur, smearing, ghosting. Do not build a plain TAA, and do not present one as an option or put it
  high in any comparison. The temporal methods wanted are SMAA T2x (2 frames, crisp) and the ML ones (DLAA, FSR 4),
  whose networks reject stale history.
- **The order**: SMAA 1x now; the motion vectors etc. are groundwork "for the future"; T2x as their first user (it is
  the cheap check that they are right before DLSS / FSR 4, which are far less forgiving of wrong vectors).
- Honest expectations, already told to the owner: at 4x the picture is drawn at 2560 x 1792 (more with native
  widescreen) and scaled down to the window, real supersampling, so any post-AA cleans up less than in a native-res
  game; FXAA vs SMAA 1x will be hard to tell apart at 4x, clearer at 2x-3x. What 4x leaves is shimmer in motion (thin
  geometry, alpha-tested cut-outs), which only the temporal methods address.

## 1. Rules for running the game (from the owner's notes; read first)

- **Don't ask before runs** (the owner, 2026-11-16), but run one at a time, spaced, as few as answer the question: on 2026-11-14 back-to-back runs took the whole PC down.
- Disc: `~/Desktop/Verdite-Project/kfiv-decomp/baserom/kfiv_us.iso`. Dumps and data folders under `~/.cache/<dir>`,
  never `/tmp` (small tmpfs).
- Byte-for-byte picture checks: headless, `--dump-frames`; graphics speed: a real window with the owner's settings
  (`~/.cache/kf4opt/run.sh LABEL SECS MAP:PLACE`, `EXE=...` for another build; it watches the game's GPU memory and the
  kernel log). Headless numbers are not the owner's (CLAUDE.md, "Testing graphics").
- The owner's settings: 4x, native widescreen, unlocked (cap 500), aniso 16, Sharpen 1.0, FXAA (the test copy
  `~/.cache/kf4hang/data/settings.json` still says `native_msaa: 2`, now ignored).
- A baseline build of main: `~/.cache/kf4_basewt` (a worktree; `git checkout --detach <commit>`, then
  `cmake --build out/linux-clang --target kf4`).
- The owner's global rules for C/C++ (`~/.claude/CLAUDE.md`): ASan+UBSan run (`out/linux-clang-asan`, ASAN_OPTIONS and
  the known noise in the `sanitizer-builds` memory), a unit test for logic testable alone, the header (interface)
  written before the implementation and shown in the report with every change to it, no raw owning new/delete, check
  every Vulkan result, extend by adding (a table entry, an implementation), name the thread a function runs on.
- Commit only when asked; never put the session URL in a commit message.

## 2. Where things are now

- Branch `feat/remove-msaa-fxaa` (pushed; commit 3624d051, not merged when this was written). Start from it, or from
  main once it is merged.
- `port/include/port/antialias.h`: the methods' table (`PORT_AA_OFF`, `PORT_AA_FXAA`, names, labels). Settings,
  `--antialias`, the overlay (Graphics, Picture: Anti-aliasing, with Sharpen under it) and `KF4_SET_AT antialias=N`
  read it. `tests/antialias_test.c` checks it.
- `port/gs/pc/pcr_antialias.h/.cpp`: `AntiAliasMethod::apply(cmd, picture)` and `AntiAliasing` (one per thread that
  makes pictures; `method_for` makes each method). FXAA: `shaders/pcr_fxaa.frag`, a copy of the picture then one pass.
- Called from `PictureOut::draw_picture` (`pcr.cpp`), on the picture at the canvas's scale, right after
  `Canvas::scanout` copies the shown surface into it, before the window scaling, the scanout ring, read-backs and
  `--dump-frames`.
- Shaders are compiled into the committed `port/gs/pc/pcr_shaders.h` by `python3 tools/build_shaders.py` (glslang at
  /usr/bin; a new shader in `port/gs/pc/shaders/` is picked up by itself; it reproduced the committed file exactly on
  2026-11-16).

## A. SMAA 1x

**Source.** The reference implementation, MIT: https://github.com/iryoku/smaa (`SMAA.hlsl`, which also compiles as
GLSL with `SMAA_GLSL_4`, and `Textures/AreaTex.h`, `Textures/SearchTex.h`). Bring it in as `third_party/smaa/` at a
pinned commit with its LICENSE (a submodule like the others, or the three files copied with the commit named), and
use it rather than rewriting the method. Ask the owner which (submodule vs copy) only if it matters to them; a copy
of three files is the simpler default.

**Shaders** (`port/gs/pc/shaders/pcr_smaa_edges.vert/.frag`, `_weights`, `_blend`; each a thin wrapper that sets
`SMAA_GLSL_4`, `SMAA_RT_METRICS`, a preset, and includes SMAA.hlsl): `build_shaders.py` needs `-I third_party/smaa`.
The vertex stage computes SMAA's offsets: either use SMAA's own vertex functions with a full-screen triangle
(`pcr_present.vert` draws one with no inputs) or compute them in the fragment shader.

- Preset: start at HIGH. Edge detection: **luma** (colour edges cost more; depth edges need the depth, part B). The
  picture is RGBA8 with the colours as shown (not linear), which SMAA expects for luma.
- Lookup textures: AreaTex (160x560, RG8) and SearchTex (64x16, R8), uploaded once per `Device` in the method's
  constructor (`create_image` with initial data; check the result).
- Images: an edges target (RG8) and a weights target (RGBA8) at the picture's size, kept while the size stays (as
  FXAA keeps its copy), plus a copy of the picture (the blend reads it and writes the picture, as FXAA does).
  SMAA's stencil trick (weights only where edges) is an optimisation; leave it out first, measure, add it if the
  weights pass is a real cost.
- Linear and point samplers as SMAA's comments ask (Granite's `StockSampler::LinearClamp`, `NearestClamp`).
- The blend pass reads the picture with linear filtering: fine for RGBA8 UNORM.

**Wiring**: `PORT_AA_SMAA` in `antialias.h`'s table ("smaa", "SMAA"), `class Smaa final : public AntiAliasMethod` in
`pcr_antialias.cpp`, a `case` in `method_for`, the overlay and `--antialias` help text, `tests/antialias_test.c`'s
names, `docs/port/PC_RENDERER.md`'s section. Settings default stays FXAA unless the owner says otherwise after
seeing both.

**Checks for A**: build, 30+ tests, AA off still byte for byte main's at 009 (5 pictures, 4x, headless), SMAA's
pictures at 009 and a cut-out-heavy spot (063: torch stands, netting) side by side with FXAA's at 2x and 4x (crop and
zoom 3x as was done for FXAA: `~/.cache/kf4aa/crop.png` method), ASan run with SMAA, one real-window run at the
owner's settings for its GPU time (the run.sh line's "picture" kind holds the AA pass). Expected cost about 0.3-0.5 ms
a picture at 4x: measure, do not quote the estimate.

## B. The groundwork for temporal anti-aliasing

What every temporal method needs, DLAA (NVIDIA DLSS SDK / NGX), FSR 4 (AMD FidelityFX SDK) and SMAA T2x alike:

1. the picture, **3D jittered** by a known sub-pixel offset each picture;
2. **depth** of the shown picture, at its resolution;
3. **motion vectors**: for each pixel, where it was in the previous picture handed out (in pixels or UV);
4. the **jitter offset** and a **reset** flag (a cut, a teleport, a map load, a menu opening: no history);
5. the **history** (the previous output), kept in presentation order;
6. for the ML ones, optionally a **reactive / transparency mask** (particles, fire, haze: things the vectors cannot
   describe) and the exposure.

Design the interface first (header, before code), shaped so DLSS's and FSR's inputs drop in. A sketch, to be judged,
not taken as is:

```cpp
// pcr_antialias.h
struct AntiAliasFrame {
    const Vulkan::Image &picture;        // RGBA8, as shown, at the canvas's scale; written in place
    const Vulkan::ImageView *depth;      // nullptr: none this picture
    const Vulkan::ImageView *motion;     // RG16F, pixels to the previous picture; nullptr: none
    float jitter_px[2];                  // this picture's offset, in the picture's pixels
    bool reset;                          // no usable history
};
class AntiAliasMethod { virtual bool apply(Vulkan::CommandBuffer &, const AntiAliasFrame &) = 0;
                        virtual bool wants_jitter() const { return false; } };
```

That is a change to the interface: name it in the report. FXAA and SMAA 1x ignore the new fields.

### B1. The hard part first: two threads make pictures

Pictures are made by **two threads**, kf4-pc and the pump (`pcr.cpp`, `PictureOut`, `Pump`), each with its own
canvas, surfaces and `AntiAliasing`, and handed out in order (`hand_out`, `hand_out_in_order`). Pictures can also be
**left out as too late** (unlocked: `pcr_late.h`). A temporal method needs **one** history in the order pictures are
shown, and motion relative to the previous picture **actually shown**. So decide, before anything else, where the
temporal pass runs:

- (a) in presentation order, after `hand_out` (one history; but the depth and motion live on the thread's canvas,
  so they must travel with the picture: images handed along with `MadePicture`, or the pass recorded by the thread
  but serialised through a shared history with a fence), or
- (b) per thread with a shared history object guarded by the order the pictures are handed out.

Measure what each costs before choosing; this decides most of B's design. Write it in PC_RENDERER.md.

### B2. Jitter

- Where: `gs_to_clip` / `gs_to_clip_h` in `shaders/pcr_common.h` (every draw's place on the target; the half-pixel
  move there is the place to add an offset, in target pixels: `PrimPush` gets a jitter field, or set 0/the picture
  header carries it).
- **Only the 3D of the shown frame buffer.** Not the 2D (FST sprites: the HUD, the menus, text), and not the game's
  own off-screen targets (the 128x128 blur surfaces, the reflection, the water's targets: jittering them only adds
  noise that is then copied into the frame). Candidate rule: draws into the frame's own buffer (`GsnInterpFrame`'s
  `draw_fbp` / `shown_fbp`; kf4-pc's own frame from its FRAME register) that are native draws or non-FST GIF
  primitives. Check it at the title, a menu, the inventory's model, under water.
- Sequence: SMAA T2x uses two offsets (SMAA's `SMAA_T2X` subsample indices); DLSS/FSR want a Halton(2,3) sequence
  over 8-16 pictures. Keep the sequence in one place (a table), picked by the method (`wants_jitter`).
- Off when the method is not temporal: pictures byte for byte as before (check).

### B3. Depth of the shown picture

- The canvas keeps the GS's Z buffers as depth surfaces (`Surface`, `depth_`), with exact Z (the depth rule:
  `Surface::zcopy`, R32F). The shown frame's Z buffer is the `ZBUF` of the 3D drawn into the shown buffer; find it the
  way `scanout` finds the colour surface, and copy the shown rectangle's depth out alongside the picture (at the same
  place: `scanout`'s rectangle and field handling).
- GS depth is **larger = nearer** and is the GS's Z (24 or 32 bits), not a linear or reversed-Z camera depth. Write
  down the conversion each method needs (DLSS / FSR take a flag for inverted depth).
- Effects drawn over the frame after the 3D (haze, brightness, the wobble) change colour but not depth: a reactive
  mask candidate (B6).

### B4. Motion vectors

The port already knows how things move: the **pictures between** move every vertex from the frame shown (t 0) to
the frame drawn (t 1) in the vertex shader (`pcr_between.h`; `GsnInterpMove` in `port/gs/shared/gsn.h`: a 3D
vertex's place, its model's place at t 0 less its place now, its model's move from t 0 to t 1; `part_moves` for
native draws; `pcr_prim.vert`, `port/render/shaders/gif_moves.h`, `vu1_*_maths.h`'s `moved()`). So a vertex's place
at the previous picture's time is computable in the same shader: evaluate the move at `t_prev` as well as `t`. Across
a game frame boundary the previous picture belongs to the previous frame's interval; at t 0 of a frame its place is
its place at t 1 of the last, so the step can be taken from the moves too (verify).

Two ways to get the vectors into an image; measure, then choose:

- **A second colour attachment** (RG16F) on the passes that draw the shown frame's 3D, written by the batch programs.
  Cheapest at run time, but it changes those passes' render passes and pipelines: `pcr_pipelines.cpp`'s records
  allow one colour attachment (`r.colours > 1` is skipped), so the list's layout (its magic) and the seed
  (`tools/pipeline_seed.py`: a tour of every map, a batch of GPU runs, spaced) must be made again.
- **The kept walk drawn again** (`Canvas::draw_again`) into a motion target with a motion program, depth tested
  against the frame's depth: no change to the main passes, one more geometry pass a picture.
- Either way, a first, cheap stage: **camera-only vectors** from the depth and the two pictures' cameras
  (`GsnInterpPicture`'s `move`/`mid`, `GsnInterpFrame`'s `screen`), in one full-screen pass. Right for the static
  world (most of the screen), wrong for moving models. Good for proving jitter, depth and history before the per-vertex
  vectors.
- **At 30 pictures a second** (no pictures between) frame_interp's moves may not be made at all: check whether
  `frame_interp.c` produces them at 30, and what it costs to have them always.
- 2D and the HUD: vector 0. Game-drawn effects (fire, haze, water's wobble, particles): no true vectors; leave them to
  the reactive mask.

### B5. History and reset

- One history image (the previous output) at the picture's size, in presentation order (B1).
- Reset on: a teleport (`--teleport`), a map load, the title, a menu opening or closing, a resolution or widescreen
  change, a quick load or state load, the first picture, and any picture whose previous was left out as too late
  beyond one (decide). Find where each of these is known (the game's frame counter, `frame_interp`'s `still`, the
  canvas's scale change) and gather them into one flag.

### B6. Proof without a method

Debug views, behind an environment variable (`KF4_AA_DEBUG=motion|depth|jitter`), written over the picture:
motion as colour, depth as grey, and a reprojection check: the previous output moved by the vectors, compared with
the current picture (where it is wrong: moving models, the 2D, effects). Picture them at 009 (still and turning),
063, a fight (moving creatures), under water. Then the owner can see that the inputs are right before any method uses
them.

## C. SMAA T2x (only if B is proven)

SMAA 1x on the jittered picture (with `SMAA_T2X` subsample indices), then SMAA's temporal resolve (`SMAAResolve`, with
`SMAA_REPROJECTION` and the motion vectors) blending the current and the reprojected previous. Its own table entry
(`"smaa_t2x"`, "SMAA T2x"). Check ghosting behind moving creatures and on the 2D (which is not jittered: it must not
blur). The owner dislikes TAA's blur: show crops in motion next to SMAA 1x before calling it done.

## Not this session

- DLAA / FSR 4 themselves. Before planning them, check (the facts change fast, and they were not checked on
  2026-11-16): the DLSS SDK's Vulkan support on Linux and its licence terms for an open-source project; whether
  FSR 4 (the ML one, RDNA 4) has a Vulkan path in the FidelityFX SDK yet, or only FSR 3.1 (not ML) does; both want
  the inputs of B (jitter, depth, motion, reset, optionally a reactive mask) and run at the picture's resolution here
  (DLAA / native AA mode, not upscaling).
- Upscaling (drawing below the window's resolution) is a separate decision; the port draws at a scale of the PS2's.

## The report at the end

What was built (A, B, C as far as they went), the interface as written and every change to it, the checks run and
not run (build, tests, byte-for-byte AA off, sanitizers, real-window speed, pictures), measured costs, and the
choices made in B1 and B4 with their measurements. Update this file's status, or PC_RENDERER.md, so the session after
knows where it stands.

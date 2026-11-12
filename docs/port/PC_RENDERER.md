# `--renderer fast` and smooth play

One plan for how `fast` draws and how smoothly it plays. It was two documents until 2026-10-29: the PC renderer's
plan (this file: P1-P6, how the world is drawn) and the smooth play plan (SMOOTH.md: steps 0-5, pipelines, pacing,
unlocked, the GPU's time). They were judged at different rates and drifted apart: the PC renderer's plan was closed
out at 170 Hz, where every spot kept up, and the smooth play plan took the GPU for the ceiling of a higher rate. Both
parts are kept below as they were written and measured, each with its corrections marked; this top says where the
whole stands.

**2026-11-07: the PC renderer is the only renderer.** The native GS (`port/gs/native/`: `native`, `modern`, `fast`,
`both`, scene scale, FSR/DLSS) is removed; players get the PC renderer, and paraLLEl-GS stays as `--renderer gs` for
developers to compare pictures against. Everything below that speaks of `fast` or the native GS is history. Still open
from item 7 below: layer Z (`--native-layer-z`) only, which the PC renderer keeps but does not apply and the overlay
does not offer: it has no layers fighting to fix, and its removal is proposed ("Colour high, the 2D mode and layer Z",
below). Anisotropy, texture detail, the presenter's best scaling and sharpening, MSAA, cut-out AA, colour precision and
the 2D mode are applied since 2026-11-11 (below) and offered again. The settings that change live go through
`port/overlay/live_settings.c`, which `KF4_SET_AT=VBLANK:NAME=VALUE,...` drives for tests (e.g.
`2400:native_widescreen=0`). Pictures are now
judged against `--renderer gs` (paraLLEl-GS). The native GS's files that item 6 names (`gsn_nd_water.inc`,
`gsn_nd_pass.inc`, `gsn_shaders.h`, ...) are in git at commit 67af1165.

## MSAA and cut-out AA (2026-11-11)

`--native-msaa` and `--native-cutout-aa` (settings.json's `native_msaa`, `native_cutout_aa`) are applied, live from
a vblank, and the overlay offers them again (Display, Picture). Built the GPU's way, not as the native GS had them:

- **The surfaces.** A colour surface drawn into with MSAA has a multisampled image beside its own (`Surface::ms`); its
  depth surface is made at the samples. The surface's own image stays what everything else reads: copies between
  surfaces, read-backs, textures read from a surface, the frame kept for the pictures between (0xF2), shader blending's
  rectangle and the scanout. Two marks say which is behind: drawn since (`ms_ahead`: resolved before a read, by
  `vkCmdResolveImage`, of only the rectangle read where that is all it reads) and written since (`ms_behind`: a new
  surface's samples made from its image at its first draw). An upload or a copy into a surface writes its image and
  the same pixels into its samples at once (`pcr_expand.frag`, a rectangle), so neither mark is needed for them. The
  pump's copies carry both images and the marks; each is copied only where it is not behind.
- **No half-pixel move with MSAA.** At one sample a vertex moves half a target pixel (`gs_to_clip`), so a pixel's
  middle is tested at the GS's point. With samples a pixel is their area: moved, a primitive on whole GS pixels (a
  sprite, a target drawn whole) covered half the samples of every pixel on its edges. With MSAA nothing moves
  (`PCR_F_MSAA`): a GS pixel is its scale's pixels whole. The 3D is shaded at the pixels' middles, so the picture as a
  whole sits half a GS pixel up and left of paraLLEl-GS's (1/8 of one from MSAA 1's at 4x).
- **DATE by the stencil** reads each sample's alpha in the pass (`pcr_date_ms.frag`, `subpassInputMS`, shaded a
  sample at a time); a GPU without sample-rate shading does DATE by a copy with MSAA.
- **Cut-out AA** (`is_cutout`): an alpha-tested batch, textured with the texture's alpha and not FST, whose alphas
  fall on both sides of the test is shaded at every sample (`pcr_prim_sample.frag`, its inputs `sample`): each sample
  is textured and tested at its own place, so the edges inside netting, fences and the torch stands are smoothed. The
  alphas it can make are the texture's least and most (kept with a texture the GPU's sampler reads; any for the rest)
  by the texture function with the vertices' alphas (a native batch's taken as 0x80). Without that test (every
  alpha-tested batch) 79,343 batches at 000 were shaded a sample at a time, with it 13,430.
- **Pipelines made ahead** (`pcr_pipelines.cpp`). A pipeline is made for a render pass's samples, so a change of MSAA
  needs the canvas's pipelines again; made by the draw that needs them, the picture stood still 0.2-1.5 s each.
  Every pipeline made for the canvas's programs is kept (Granite's compile hook, `cmake/granite_pipeline_warm.inc`)
  with its pass's attachments, and when the samples change each is made again at the new count, with the per-sample,
  DATE and expand variants, on a thread of its own; the canvas draws at the old count until they are made (a change at
  009 at 240: 42 pipelines in 1.2 s on that thread, 57 vblanks at the old count, no draw waiting). Only what was drawn
  is known: a run started with MSAA makes its first pipelines as at 1x (the PC renderer has no pipeline list yet).
  `KF4_PC_WARM_LOG=1` names each pipeline made.
- **Granite** hashed a render pass by its formats, not its samples: a multisampled pass was given the single-sampled
  one of the same formats. `cmake/granite_msaa.cmake` adds the samples.

Checked (Windows, RTX 5080): with MSAA 1 the pictures at 000, 009, 009:2 and 063 byte for byte the build before
(1x: 48; 4x native widescreen: 16); ctest's 11 tests. The map tour (000, 009, 014, 022, 026, 035, 112 at 4x without
native widescreen) against `--renderer gs`, MSAA 4 with cut-out AA: no view broken. Views over 0.4% (blurred): 1 at
MSAA 1 (014 v5, the water, as before), 9 at MSAA 4: 014 v5 again, 112 v1, v4, v6, v7 (1.1-2.0%), whose fine wall
texture shows the half-pixel shift above (side by side the same picture), and 026 v0, 009 v0, 035 v2, 014 v6
(0.44-0.69%: the shift, and 026's fire). MSAA 4 against MSAA 1 view by view: under 0.4% (blurred) but those four of
112. Edges smoothed (the torch stands at 063, the cone at 026); water, reflections, shadows and the HUD as at MSAA 1.
The four spots at MSAA 4 against MSAA 1 (4x, native widescreen): 0.03-0.13% (blurred). The sky at 000 and 063 at 4x,
native widescreen, MSAA 2 (issue 96): no lines, by eye and by a search for rows and columns that stand out from both
neighbours (which finds a line 10 levels darker drawn in); `--native-colour high` is not applied yet, so not with it.
Nor in the tour's views at MSAA 4: the rows and columns found are the walls' own edges, there in paraLLEl-GS's too.

GPU time, the renderer's timestamps (`KF4_GSN_GPU_TIME=1`), a whole run to 009 at 240 (2,400 vblanks, 3,100
pictures), 4x native widescreen, alternated three times each, measured with another game running on the machine
(so only the ratios mean much; one MSAA 4 run took 7 times its least, plainly while the other game drew). A picture,
the least of the three runs and the median:

| | least | median |
|---|---|---|
| MSAA 1 (the build before) | 0.79 ms | 0.82 ms |
| MSAA 2, cut-out AA | 1.26 ms | 1.38 ms |
| MSAA 4, cut-out AA | 1.72 ms | (6.8 ms, contended) |
| MSAA 4, cut-out AA off | 1.52 ms | 1.76 ms |

So about 1.6 times the GPU's time at MSAA 2 and 2.2 at MSAA 4, cut-out AA about 0.2 ms of it at 4. By kind (least
runs, MSAA 4 against 1): the native draws about 2.4 times, the copies and resolves 4 times (the pump's copies of the
samples each frame; the depth surface at 4 samples is the largest), and the expands (`surface`) 0.03 ms a picture.

Still open: the half-pixel shift against paraLLEl-GS could be taken back by reading the 3D's inputs half a pixel up
and left (`interpolateAtOffset`); points and lines are still not drawn; MSAA 8 not tried.

## Colour high, the 2D mode and layer Z (2026-11-11)

The last three settings the native GS had (issues #102, #104, #105), each checked first for whether the PC renderer
has the problem it fixed. The native GS's are at 67af1165 (`gs_frag.h`, `gsn.cpp`'s `texel_rect_2d` and "Z and
layers", `narrow.frag`).

**The 2D mode (`--native-2d smooth`, the default; #104): applied.** At a scale above 1 the 2D's textures read nearest
(MMAG nearest at level 0) and magnified on the target (a texel at least a pixel across) are read with a Catmull-Rom
cubic at each pixel's own place (`pcr_prim.h`'s `smooth_2d`, push flag `PCR_F_SMOOTH_2D`), not as blocks of texels:
letters with smooth edges, the same picture at every scale. The taps stay inside the texels the primitive covers, so a
letter's cell of a sheet of letters does not take in the letters beside and below it (the native GS's text-bleed
fix): each vertex carries its primitive's texel rectangle (`Vertex::rect`, 8 bytes more, `a_rect` / flat `v_rect`),
made on the CPU for sprites and for 2D triangles (FST, or one Z and one Q at the three corners: the menus draw their
text and panels as such triangles with S, T and Q; only FST at first, and the menus' text stayed blocky). The result is
kept inside the four texels around the place (no ring). The place is moved half a pixel's change of the coordinates,
where nearest puts a texel's middle (none with MSAA: no vertex move there). Not the 3D (no rectangle), not textures
read from drawn surfaces, not native draws. Nothing at 1x. In a picture between the rectangle is the frame's (the
primitive's texels do not move with it).

**Colour high (`--native-colour high`, opt-in; #102): applied, default as it was.** The colour surfaces are made
R16G16B16A16_UNORM (`colour_format`; their multisampled images, copies, the pump's copies too), the colours'
arithmetic in floats in the shader (`PCR_F_HIGH`: texture function, fog, premultiply, the shader's blend from the
destination with its fraction; the sampler's filter and the 2D cubic keep their fraction), with the native GS's
level-keeping (`gs_floor`, `gs_shift7`: the GS's floors take half a step on average, so does the float path, else a
dark scene comes out a step brighter); alpha stays the GS's integers. The picture is dithered to 8 bits as it is
scanned out (`pcr_narrow.frag`, interleaved gradient noise, a whole number unchanged; the lower field's half picture
too). Read-backs to the GS's memory are blits to RGBA8 (rounded). It changes from a vblank (`apply_colour`, surfaces
made again by a blit; the kept frame let go); a GPU without 16-bit colour attachments with blending stays exact. The
pipelines made ahead for an MSAA change are made against the format of the run's first change (the other precision's
are made as drawn). `KF4_SET_AT` takes `colour=` and `native_2d=`.

**Layer Z (`--native-layer-z`; #105): not applied, proposed for removal.** The native GS rounded each pixel's Z to a
whole number as the GS does, so a layer blended over its surface (a second mesh, GEQUAL) failed where the two
meshes' Z rounded apart, and tolerant was its way out. The PC renderer keeps the GPU's Z with its fraction and no
rounding, and its layers are not seen fighting:
- A tolerant mode built the GPU's way (a `gl_FragDepth` variant of `pcr_prim.frag` testing the colour pass a half Z,
  or a layer's pixel one Z and half its slope, nearer; ties in two phases, everything else blended Z first) changed
  the 4x tour views (maps 002, 004, 009, 016, 020, 026, 035, 040, 099, 102, 112, 115, 96 views) by at most 0.05% of
  pixels (0.02% blurred), and map 000 (12 views) by at most 0.15%, nothing against paraLLEl-GS (the blurred share the
  same to 0.01%). What it did change was worse: map 004 v1's tree (blended alpha-tested leaves) lost the leaves drawn
  behind (Z first keeps only the nearest's colour within the slack), light seams through the foliage; a dark line down a
  wall's corner at 026 v1; the torch flames at 000 v0. At 1x at the four spots: 0-65 pixels a picture.
- The Equipment screen's figure (the case the native GS's tolerant mode broke and then had to special-case: the menus'
  Z32 buffer, the second pass over the model): drawn by the PC renderer as by paraLLEl-GS, no patches.
So the code was not kept; the setting stays in settings.json and on the command line, does nothing, and the overlay
says so. Proposed: remove `--native-layer-z` and `native_layer_z` (the default "tolerant" there is the native GS's).

Checked (Windows, RTX 5080): with `--native-2d ps2` (and colour exact) the four spots byte for byte the integration
branch's (1x, 192 pictures); at the defaults too at 1x (the 2D mode does nothing there). At 4x the 2D mode changes
the menus' and the pause screen's text and panels (evidence: smoother letters, no specks from the cells beside); the
HUD in play at 000 not at all (its textures are not read nearest-magnified). Colour high at 4x: every pixel within 4-6
levels of exact, the mean 0.4-0.6 of a level brighter; no banding to see in exact at the spots (stretched crops of the
sky at 099 and a firelit wall at 009 look the same in both), and the map tour at 000 against paraLLEl-GS the same
(mean difference 2.36 against 2.37). The owner's earlier finding stands: high is hard to tell from exact, so it stays
opt-in.

GPU time a picture (`--stats` with `KF4_GSN_GPU_TIME=1`, `KF4_PRESENT_SIM=240`, 009 at 240, 4x with native
widescreen, a run to vblank 2600, the variants alternated three rounds; **contaminated**: the owner was playing another
game on the same GPU, so only large differences mean anything):

| ms a picture | rounds | least | median |
|---|---|---|---|
| base (2D ps2, colour exact) | 2.136, 2.165, 1.995 | 2.00 | 2.14 |
| 2D smooth | 2.203, 2.140, 2.005 | 2.01 | 2.14 |
| colour high | 2.095, 2.268, 1.869 | 1.87 | 2.10 |
| MSAA 2 | 2.846, 2.751, 2.164 | 2.16 | 2.75 |
| MSAA 2 + colour high | 2.343, 2.631, 2.481 | 2.34 | 2.48 |

The 2D mode costs nothing measurable here (its draws are few). Colour high's time a picture is within the noise, but
its runs drew 20-30% fewer pictures in the same run (3,515-4,346 against 4,762-5,930: pictures left out as they fell
behind), and its copies (`copy`, `texture copy`) about twice the time: the 16-bit surfaces are twice the bytes. Not
measured cleanly; to be measured on a quiet machine before anyone recommends it.

Still open: colour high's level is a little brighter than exact (0.4-0.6 of a level); textures read from drawn
surfaces and the GS's own read (the region modes, the CLUT) keep whole numbers in high; the 2D mode does not reach 2D
drawn as perspective triangles.

## Texture detail, anisotropy and the presenter's scaling (2026-11-11)

The three settings the native GS had (`--native-lod`, `--native-aniso`, `--present-scaling best` with
`--present-sharpen`) done the GPU's way, from the native GS at 67af1165 (`gs_frag.h`'s `sample_texture`,
`gsn.cpp`'s `present_scale`).

**What the game gives.** Its mipmaps are its own: TEX1's MXL and the MIPTBP levels, which the texture store decodes
with level 0 (`DecodedTexture` has every level). In a census of the map tour (64 maps, 2 places x 2 headings) 41
maps draw mipmapped textures and 23 none (000, 006, 007, 011, 013 and others); where there are some, 2-25% of the
sampled batches (100, 107, 109, 031 the most). Every one is MMAG linear, MMIN 4 or 5 (bilinear in a level, nearest or
linear between them). No 2D texture has levels.

**Texture detail** moves the level the GS picks from Q (pcr_prim.h's `sample_texture`, both the sampler's read and
the GS's): `scale` (the default) by -log2 of the smaller of the two scales, `full` by -64 (level 0), `ps2` not at all.
Only where the GS's level is above 0, so the choice between MMAG and MMIN stays the GS's (a level moved to 0 or below
is held at 1/256 for the sampler: its minifying filter at level 0). At 1x `scale` is the GS's own level.

**Anisotropy** is the sampler's (`anisotropyEnable`, up to `aniso`), on the samplers of mipmapped bilinear textures
only (`anisotropy_for`); the 2D, the region modes and textures from surfaces never have it. With it the shader takes
the texture coordinates' change a pixel across and down (in `main`, before any discard), and where the footprint is
long enough that its long axis over the reads gives a level finer than the GS's, reads with `textureGrad` and the
footprint as it is (the hardware's reads along it); elsewhere `textureLod` with the GS's level as before. So it is
never blurrier than without it. (A first try stretched the footprint where its short axis was under a texel, to keep
the sampler minifying: that spread the reads past the footprint and blurred floors at 4x; dropped.)

Both reach the shader in tex1's spare bits (anisotropy 10-12, the bias 16-31 in 256ths), set when a batch binds its
texture, so a kept walk draws again with what it was bound with; the canvas takes the settings at each vblank
(`Canvas::set_sampling`). `--stats`: `pc canvas textures: ... (N mipmapped, N anisotropic)`.

**The presenter's scaling.** The presenter (present_vk.cpp, SDL_Renderer's video.c, and `--dump-present`) already
asked for it through `pcr_set_present_scale`, and copies `GsnScanout.scaled` a pixel a pixel when it is there; only the
PC renderer's side was missing. Now each picture copied into the scanout ring is also drawn into the slot's own
scaled image with the native GS's two shaders (`pcr_present_scale.frag`: Catmull-Rom, stretched when made smaller,
on linear light; `pcr_present_sharpen.frag`: contrast-adaptive) and `pcr_present.vert`'s triangle
(`PresentScaler`, pcr.cpp); a read-back with `GSN_WANT_SCALED` reads it. GPU time kind `present`.

**Checked** (Windows, RTX 5080):
- Defaults, 1x (the four spots, 240, vblanks 2300/2450/2600/2699: 64 pictures): byte for byte main's.
- Defaults, 4x: the picture changes on maps with mipmaps, as it should: `scale` was the setting's default and is now
  applied (main drew the GS's level at every scale, as `ps2`; `--native-lod ps2` is byte for byte main's on the
  tour's 48 views). Maps without mipmaps are as before.
- The map tour at 4x (maps 002, 008, 027, 031, 100, 107; 2 places x 4 headings) against `--renderer gs`: every view
  under 0.02% (blurred) for main, `scale`, `full`, `ps2`, anisotropy 16 and `ps2` with anisotropy 16; nothing drawn
  differently. By eye: `scale` sharp where main was blurred at 4x, a little grain at a grazing distance (031's floor);
  anisotropy 16 keeps the detail without the grain; with `ps2` it sharpens the floors at a grazing angle. The HUD's
  gauge byte for byte with every setting. `full` and `scale` are near the same at 4x (the game's textures have few
  levels: -2 already reads level 0 almost everywhere).
- `--dump-present`: 4x into 1280 x 896 (1195 x 896 at 4:3) with and without `--present-sharpen 1`, 1x into 1920 x
  1344; main's `--dump-present` gave the picture at its own size.
- In a window the default picture changes too: `best` has been the scaling's default all along, and the presenter
  blitted bilinear while the PC renderer made no scaled picture; now it gets the cubic (and sharpening, at the
  owner's settings).
- The unit tests (11, run one by one: no ctest on this machine) pass.
- Not checked: motion (shimmer is judged from stills), the Vulkan presenter in a window (no window opened), the
  sanitizers.

**GPU time a picture** (`--stats` with `KF4_GSN_GPU_TIME=1`, `KF4_PRESENT_SIM=240`, 4x with native widescreen, 240,
vblanks 1900-3100; variants alternated, three rounds; measured while another game ran on the same GPU, so only the
larger differences mean anything):

| ms a picture | main | defaults (`scale`) | anisotropy 16 | anisotropy 16, best to 2560 x 1440, sharpen 1 |
|---|---|---|---|---|
| map 009 | 1.99, 1.73, 1.90 | 1.96, 2.02, 1.23 | 2.55, 2.15, 1.90 | 2.62, 1.89, 2.38 (of it the scaling 0.73, 0.52, 0.61) |
| map 100 | 2.06, 2.00, 2.20 | 2.05, 2.49, 2.25 | 2.33, 2.34, 2.18 | |

Texture detail costs nothing measurable; anisotropy 16 perhaps 0.1-0.3 ms (009 has few mipmapped batches, 100 the
most of the census); the best scaling with sharpening is its own GPU kind, `present`, 0.5-0.7 ms a picture from 4x
to 2560 x 1440 (two full-screen passes, the cubic's reach 4 x 4 pixels of the picture at that ratio).

## Against paraLLEl-GS: the map tour (2026-11-07)

The first whole-game comparison of the PC renderer with `--renderer gs`: `tools/map_tour.py` (64 maps, 3 places x 4
headings, 760 views) run three times at c2cb20c3 (gs; pc at `--native-scale 1 --no-native-widescreen`; pc at
`--native-scale 4 --no-native-widescreen`), compared by `tools/tour_compare.py` (pictures paired, the 4x one brought
down to 640 x 448, ranked by the share of pixels over 24 levels after a 3 x 3 blur). Every run ended, every view's game
state the same in both. Outside the cases below, views differ in under 0.4% of pixels (filtering and edges). Found:

1. **Objects not drawn.** The Queen's Palace (022) doors (an empty doorway where gs has the carved door: views 2, 11),
   the torch pedestals of 026 (v0) and 031 (v10, a row of them: their flames are drawn, the stands not), the
   standing stones of 027 (v6), the crystal flower of 115 (v5). The owner sees the same in play.
2. **The HUD's health gauge not drawn**: only its ring's outline, at 55 views of 8 maps (016 v4-7, 018 v4-5,7,
   022 v4-11, 024 v0,3-4,7-8,10-11, 040 nine views, 102 v0-3,8-11, 103 all twelve, 104 v0-3); right at every other map.
3. **The water of 014** (the Heart of the Forest, v5): pc draws it pale and opaque, gs dark and clear (17% of the
   picture). Fixed 2026-11-11, below.
4. **Seams at 4x**: map 035 at 4x shows hairline lines across the wall and floor (a vertical line near the middle,
   a horizontal one on the floor) that 1x and gs do not. Likely the same as the lines the owner sees in the sky.
5. **Small**: a rock's edge at 112 (v10) cut differently near the camera. Fixed 2026-11-11, below.

Not covered by the tour: menus, combat, creatures' effects, motion and the pictures between, the owner's full settings
(native widescreen, MSAA): "Against paraLLEl-GS in motion" below.

**1 and 2 fixed (2026-11-07, the same day): one cause.** The canvas recorded copies inside an open render pass:
`write_pixels` (an upload written into a colour surface) and `grow` ended the open pass only when it drew into the
surface they wrote. The game uploads into its 128-wide targets (pages 450 and 466, the water's) while the frame's pass
is open; the blit went into that pass (not allowed in Vulkan), and RADV lost the frame's draws around it: whatever the
frame drew between the uploads (objects, doors, the gauge). Both now end any open pass. Read back from the GPU after
each batch the surface was right, so the bug hid from any check that ended the pass (that is how it was found:
`KF4_DBG`-style probes in a scratch copy, not kept). After the fix: both tours against gs, 760 views each, every view
under 0.4% (blurred) but 014 v5 (3 above) and 112 v10-11 (5); 4x adds 035 (4). ctest 14/14; ASan + UBSan at 022,
026, 115: known reports only (SoundDriver_Remote, decomp function-pointer casts), nothing in port/. Not run: TSan,
the Vulkan validation layer (not installed here: `vulkan-validation-layers`; it would have named this at once).

**4 fixed (2026-11-08): every vertex moved half a pixel of the target, not half a GS pixel.** The GS draws a pixel
when its integer point is inside a primitive and Vulkan when the pixel's middle is, so the vertex shaders moved every
vertex half a GS pixel. At 1x that is exact; at 4x it put a GS pixel's four pixels around its point (-0.375 to
+0.375), so a primitive whose edge is on a whole pixel (a sprite over a whole target) left the first half GS pixel
undrawn: two columns and two rows at 4x. The game draws its lava (035's floor, page 450) into a 128 x 128 target with
such sprites and repeats it over the floor, so the undrawn columns and rows showed as lines where it repeats (found with
a GS capture: `kf4-gsreplay --pixel` named the floor's draw, its texture the target; the target's copy shown on the
screen had its columns 0-1 empty). Now `gs_to_clip` (pcr_common.h, for pcr_prim.vert and the three native vertex
shaders) moves a vertex half of the target's own pixel (1 / scale of a GS pixel): a GS pixel's first pixel at any scale
is tested at the GS's own point, a primitive covers whole GS pixels, and a surface read back at its scale
(`read_surface`) reads pixel j at pixel j, not two pixels up and left. The 4x picture as a whole sits 3/8 of a GS pixel
up and left of where it was (no longer centred on the GS's points); at 1x nothing moves.

Checked: the 1x tour byte for byte the build's before (760 views; 039 v0-1 differ by up to 5 levels, and differ run to
run with the same build); the 4x tour against gs: views over 0.4% (blurred) 48 → 11, the median 0.06% → 0.00%, 035 at
0.33% (was 0.9-1.2%); the mean difference a little higher (2.98 → 3.19, the 3/8 pixel at sharp edges: 020 v11's
bright magic circle, 2.70%, is the most). Still open: 014's water (3), 112's rock (5). ctest 14/14. Not run: the
sanitizers (a shader change and one push constant), the owner's full settings (native widescreen, MSAA).

**3 and 5 fixed (2026-11-11; issues #94, #95; measured on Windows, NVIDIA RTX 5080).** Three causes, all in the
GIF path (the tour's 60 Hz runs draw no native draws: `port_gs_native_draws_now`).
- **014's water, two causes.** The water is two passes (`kf4-gsreplay --pixel` on a capture of the view): the
  reflection, drawn first into a 256 x 256 PSMCT16S target (FBP 350) and blended over the water at half (ALPHA
  `(Cs - Cd) * As + Cd`, As 0x40), then a 4-bit texture added. (a) `surface_texture` read only CT32 and CT24 targets
  as textures, so the reflection pass went untextured: half grey 0x80, the pale opaque water. A 16-bit texture is
  now read from a surface of its own format, as the GS reads it back from memory (each colour's top 5 bits, alpha
  TEXA's by bit 7: `PCR_F_SURFACE_CT16`). (b) That showed the water 9 levels darker than gs: the reflection is
  Z-tested (GEQUAL, ZMSK) against ZBUF page 280 at width 4, which the frame's clear has just zeroed at width 10.
  Depth surfaces are keyed by base, width and format, so the width-4 image kept Z from earlier frames and most of
  the reflection failed. A clear (sprites writing one Z, ZTST ALWAYS, over whole rows of pages) now clears the other
  widths' depth surfaces lying wholly in those pages (`clear_depth_aliases`; the stats line counts them: about two
  a frame at every map). Partial overlaps (the 128 x 128 water targets write Z = max through ZBUF 280 at width 2,
  mid-frame too) are still not carried between widths.
- **112's rock.** Its near triangles have corners' Z up to 0xF39C0F over a Z16S buffer. The GS (and paraLLEl-GS:
  `ubershader.comp`) clamps each pixel's interpolated Z; the vertex shader clamped the corners, lowering Z across
  the triangle, and it failed Z tests gs's passed. Such a triangle is now cut on the CPU where its Z reaches the
  format's largest (`add_cut_at_zmax`): the part beyond has every corner at or past it, where the corner clamp is
  exact. Not done for a picture between's moved corners (a cut point has no kick) nor native draws (Z made on the
  GPU): those still clamp the corners, so with pictures between or native draws on this edge can still differ.

Against gs, blurred share: 014 v5 17.08% → 0.14% (1x), 17.04% → 0.00% (4x); 112 v10 2.04% → 0.20% (1x), 1.82% →
0.05% (4x); 112 v11 0.90% → 0.27% (1x), 0.73% → 0.07% (4x). Byte for byte the build's before: the 1x tour of 000,
004, 009, 016, 020, 026, 035, 040, 102, 115 (120 views), and 000, 009, 009:2, 063 at the owner's settings (4x, native
draw, 240 Hz, pictures between included). On this machine 112 v4, v6 and v7 are 1.1-2.6% at 1x (triangle by
triangle texture detail, the same before these changes, under 0.4% at 4x); Andrew's AMD tour had them under 0.4%,
so likely the platform's sampling (left for the Windows/NVIDIA check).

## Windows, NVIDIA and the Vulkan validation layer (2026-11-11)

First runs of the PC renderer on Windows 11 with an NVIDIA RTX 5080 (driver 610.88; the `windows-clang` preset,
llvm-mingw), and the first under the Khronos validation layer. The build, boot_test, frame_test (pc) and the unit
tests pass as they are. The map tour (maps 000, 009, 014, 099, 112, 2 places x 4 headings) against `--renderer gs`:
paraLLEl-GS draws the same pictures on the NVIDIA and on the machine's AMD iGPU (`GRANITE_VULKAN_DEVICE_INDEX=1`),
but pc on NVIDIA differed at 1x: 112 v4-7 by 1.1-2.6% (blurred), 009 v0 by 0.8%, where pc on the AMD was under 0.4%.

**Fixed: FST texture coordinates rounded to 1/256 texel** (`pcr_prim.h`'s `texture_uv`). Not the 3D draws:
their texture coordinates came out the same on both GPUs (checked with the shader writing them out). The difference
came in the game's 2D passes that draw the frame back over itself in 64-pixel columns (FST, a texel a pixel), where a
pixel's UV is often a whole texel: the GPU's interpolation lands a hair either side of it, NVIDIA's below, so floor
(and the sampler's nearest) took the texel before, and the picture moved a pixel over whole triangles. 112 v7 against
gs: NVIDIA 27,025 pixels over 24 levels -> 17, the AMD iGPU 2,168 -> 17 (AMD had some of it too). After it the 1x
tour is under 0.4% at every view but 014 v5 (the water, item 3 above), and NVIDIA and AMD are the same (1x, and 4x
at 009 and 112).

**The validation layer** found, at map 112, 009 and the starting map (pc, 1x and the owner's settings at 240):
- VUID-VkWriteDescriptorSet-descriptorType-00338 and VUID-VkFramebufferCreateInfo-pAttachments-00879: the colour
  surfaces lacked `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT`, which DATE's set-up reads them as. Fixed.
- VUID-vkCmdDraw-None-09600 (at the submit): `write_pixels`' staged image left READ_ONLY_OPTIMAL where Granite took
  it as GENERAL. Fixed.
- With synchronization validation (`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`): SYNC-HAZARD-WRITE-AFTER-WRITE, a
  first clear racing the layout transition Granite's `create_image` records for a new image (its barrier's second
  scope is NONE). Fixed by cmake/granite_layout_sync.cmake (a barrier on every platform; no driver showed it).

After them: no messages at maps 000, 009, 009:2 and 063 at the owner's settings at 240 (native draw on), nor in two
map tours under the layer (the five maps, 1x, and 4x with native draw); sync validation clean at 009 and 009:2.
`--renderer gs` cannot be checked here: paraLLEl-GS asks for VK_EXT_descriptor_heap, which the layer (1.4.335 and
1.4.341) does not validate yet (it reports ranges it made up, then crashes in the layer or the driver); with
descriptor heaps and buffers left off (a scratch change) it still crashes under the layer. Developer-only, not
followed.

Also: `--teleport` with a vblank needs MAP:PLACE (`1900:9` is map 1900, place 9: use `1900:9:0`); `pace_test.py`
takes places (`--maps 0,9,9:2,63`), finds the Windows build and runs `--invincible` (at 009:2 the creatures kill the
player within the run). A crash on Windows now names the module and offset and the callers (`[port] crashed: ...
(nvoglv64.dll+0x...)`). Pace: not measured cleanly (the machine was in use: a game running beside it). Not run:
windowed (the presenter's swapchain), Intel, the sanitizers on Windows.

### The validation layer: how to run it

Linux: install the distribution's package (`vulkan-validation-layers`, or the LunarG SDK), then
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`. Windows without an installer: build it with the same llvm-mingw
(`git clone --branch vulkan-sdk-1.4.341.0 https://github.com/KhronosGroup/Vulkan-ValidationLayers`, then
`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
-DUPDATE_DEPS=ON -DBUILD_TESTS=OFF -DUSE_MIMALLOC=OFF` and `cmake --build build`: about ten minutes), put
`VkLayer_khronos_validation.dll`, its `.json`, `libc++.dll` and `libunwind.dll` in a folder and point
`VK_ADD_LAYER_PATH` at it (no registry, no admin). Not MSYS2's build: under it the game's handle count grew by about
20,000 a second and the process hung at exit. Then, on either system:

```sh
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_KHRONOS_VALIDATION_LOG_FILENAME=vv.txt          # (else the messages go to stdout: map_tour's NNN/stderr.txt)
export VK_KHRONOS_VALIDATION_DUPLICATE_MESSAGE_LIMIT=3
export VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true            # synchronization validation (slower)
kf4 --disc ... --headless --no-audio --dump-frames N ...
```

Granite enables the layer itself only in a `VULKAN_DEBUG` build (not ours), so the variables above are the way.
Under the layer the game runs far slower: a few hundred pictures between in a 17-second run at 240.

## Against paraLLEl-GS in motion (2026-11-11)

What the map tour left out (issue 112): `tools/motion_compare.py` plays 12 cases by pad script after a new game
(`tests/motion/`), each by gs and by pc with the same input, and compares every dumped vblank's pictures as
`tour_compare.py` does; the per-frame log at each vblank is the same in both runs (it was at every vblank of every
case). The cases: the title and the opening's messages, every page of the menu (Item, Magic, Equipment with a weapon's
model, Status, Journal, Arrange, System and Load), an item picked up, combat (a club put on, map 002, a Maneater put
before the player with `KF4_FX=creature=0`: swings, the trail, its attacks), spells 3 and 16 cast, creatures (map 102's,
the Fire Genie and Larvae of 025, the Salamanders of 016, the Watchers of 022), and the under-water and MP drain screens
(forced by `KF4_FX`). Each at 1x and at 4x with native widescreen and `--native-msaa 2`, and each at 240 (pc only:
`flash_find.py --frames` on the pictures between; the game frames at 240 against the same vblanks at 30). Run at
cf80b086 on Windows (RTX 5080). MSAA is passed but not applied on main (item 7 above), so it is not covered.

Found:

1. **The under-water and MP drain screens lose the GPU.** `KF4_FX=water` or `drain` (the wobbling screen drawn from
   the copy of the frame at 0x2BC0, `Game_DrawWobblingScreen`) gives `vkQueueSubmit2 failed (code: -4)` (device lost)
   from the first frame it draws, every picture black after, at 1x and 4x, with native draws off too, and with each of
   the `KF4_PC_*` switches at 0. The other forced effects (copytex, prev, aim, tint, fill, down, bright40, fade, dim)
   do not. Real deep water (029) and the Queen's Palace drain were not reached; they draw the same.
2. **The HUD's MP ring is never drawn.** Its blue arc is drawn with DATE (DATM 1) where the MP bar's sprite set
   alpha bit 7 with FBA (`Hud_Draw`, sprites 5 and 6). By the stencil the set-up (`pcr_date.frag`) finds bit 7 clear
   under the whole arc, so nothing is drawn; `KF4_PC_STENCIL=0` (DATE by a copy) draws it as gs does. The same build
   with DATM inverted in the set-up draws the whole arc: the set-up reads the target's alpha without the bit the
   batch before wrote. Not the dynamic state (`KF4_PC_DYNAMIC=0`) nor a pass shared with earlier draws (a fresh pass
   for each DATE batch). Every play picture with MP has it (0.3% of the picture: under the tour's 0.4%).
3. **A black picture as the menu opens**: the vblank after CIRCLE (1805 of the case), at 1x and 4x, at 30 and 240,
   with and without whole frames. gs shows the last game frame there. Not `UpperField` (c043b057).
4. **The menus at 1x sit one GS pixel left** of gs's (and sharper: gs shows a field a vblank); at 4x they line up and
   differ by the smooth 2D only. 5-13% of each menu picture at 1x, 1-4% at 4x. The title: half a line apart (the field
   screens, by design).
5. **Small**: at 4x, textures finer (`--native-lod scale`) and the 2D smoothed, as meant. Spell 3's wisps at 240:
   29 of 240 pictures between flagged, each a translucent wisp moved between its game frames (looked at: no shards, no
   holes, no wrong places).

Clean: the pick-up, combat (the trail, the creature, the hits), spell 16, the creatures' cases, the teleport's fade
and the opening, under 0.4% blurred at every vblank but the HUD ring; no picture between flagged outside spell 3; the
game frames at 240 the same as at 30. Not covered: breath and glow effects (none of the creatures placed used one in
its window), spells 36, 61 and 76 (nothing showed in their window), the shop, a save point's save screen, doors and
stairs (the teleport's fade only), real deep water.

**1, 3 and 4 fixed, 2 is #121's (2026-11-11, branch fix/motion-bugs).**

1. **Device lost: a copy past an image's end.** The validation layer with GPU-assisted validation
   (`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT`) named it at once:
   VUID-vkCmdCopyImage-dstOffset-00151, a 128 x 2048 surface copied 32 rows down into another 128 x 2048 one. Not the
   1024 x 512 texture past VRAM's end (that read is refused as before: no surface of its width). The wobbling screen's
   copy of the frame (page 350, 640 x 448) lies over the water's two 128-wide targets (pages 466 and 468, which map
   000's lava uses), so every frame they were let go and made again, 466 taking 468 in a page row down. A draw whose
   depth surface was taller than its colour surface grew the colour one to match; the two targets share a Z buffer, so
   each making-again grew the pair by 32 rows, to 2048 within a few seconds, and then the take-in copy ran past the
   end. Now a take-in copies only what fits, and a colour surface is never grown for its depth surface (a Vulkan pass
   is as large as its smallest attachment; a shorter depth surface is still grown). `underwater` and `drain`: under
   0.4% at 1x, 4xw at most 0.43% (4x's finer textures). It needs only the under-water bit (`KF4_FX=water` sets the
   game's own PlayerState status bit 39, so the game draws as in real deep water) where those targets are drawn; real
   deep water and the real drain were not reached here.
2. **The MP ring: the missing `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT`.** #121's commit 79b08c29 alone (on this branch)
   draws the ring as `KF4_PC_STENCIL=0` does; all of #121 draws it as gs (no pixel over 24 levels in the HUD; the rest
   is #121's FST rounding). The ring shows only with MP: `--cheats all` (a new game has none).
3. **The black picture: two surfaces of one memory.** Play grows the frame's surface (page 0) from 224 to 928 rows,
   over the title's second field buffer (page 70), which was kept as a surface of its own; the menu draws into page 70
   (that old surface, grown) and the vblank after CIRCLE shows page 140, found in the old surface's new, empty rows. A
   colour surface grown now takes in the surfaces that begin in its new pages, as a surface made does. v1805: 62.4% ->
   0.31% at 1x, 67.0% -> 0.33% at 4xw; no picture between flagged at 240.
4. **The menus a pixel left: S/T/Q read a hair below a whole texel.** The menus end with the brightness sprite, the
   buffer drawn back over itself 1:1 by S/T/Q (and the background is copied the same way); S/Q times the size lands
   on a whole texel and the GPU's lands just below it, so floor took the texel before. Found with a GS capture under gs
   (`--gs-capture-at`, `kf4-gsreplay --draw`): gs's frame buffer lines up with its picture, pc's picture with it only
   a pixel over. S/T/Q coordinates are rounded to 1/256 texel (`pcr_prim.frag`). Menus at 1x: median 5.4% -> 0.9%,
   best offset against gs 0,0; at 4xw still lined up. What is left at 1x is gs's softer field picture. The four spots
   change by up to 3 levels (bilinear's 1/16 steps), and against gs are as before.

Spots (000, 009, 009:2, 099 at 240): byte for byte main's after 1 and 3.

## Step 3's last two built (2026-11-11; issues #97, #98): the reflection's water and the screen passes

`[stats] pc native draws not drawn yet` now lists nothing: every native draw is the PC renderer's. `kinds()` adds
`water` and `passes`, so `--native-draw on` (and the owner's setting) draws both. Branch `pc/native-water-passes`, on
top of #118 (fix/tour-water-rock: the water reads the reflection's 16-bit target, which #118 made right; without it
014's water is pale with either path). Measured on Windows, NVIDIA RTX 5080.

- **The water (`RD_PROGRAM_RIPPLE`, maps 008, 009, 014).** No VU1 program: Wobble_DrawRippledModel makes each vertex
  on the EE (ripple.hpp says how). The CPU makes the record (the part's vertex matrix in VU0's arithmetic, the 32
  sines) and the strips' ADC, which needs the places (`port/render/ripple.cpp`); the GPU makes each corner's place,
  S and T (`shaders/pcr_ndripple.vert`, the maths in `port/render/shaders/ripple_maths.h`, a new `NativeProgram`).
  In a picture between the places are moved by the picture's camera and S and T made again from the moved ones, as
  the native GS did (the owner's decision 3 of 2026-10-23: the reflection stays under the water). `ripple_test`
  checks the record and the ADC against Wobble_DrawRippledModel's maths as nd_water.c's KF4_ND_WATER_CHECK writes it,
  and the shader's S and T arithmetic (copied to C++) against the game's floats for every X and Y of 16 bits, toward
  zero and to nearest.
- **The passes (`RD_PROGRAM_PASS`).** Each pass is written to the GIF front end as the PS2 path's packets
  (`port/render/pass_draw.cpp`, the native GS's `gsn_nd_pass.inc` again): the same register writes and primitives,
  so the state after is the PS2 path's. Only the flat fills are one sprite instead of ten strips. The brightness and
  the tint stay ten strips: as one sprite reading the frame one texel a pixel, the GPU's interpolated texel at a GS
  pixel's point now and then came out a little under the whole texel and read the pixel to the left (125,000 pixels
  of the brightness pass moved by one at map 000, worse against gs than the strips). In a picture between a pass is
  paired by its identity (pass, call site, count in the frame) with the frame before's (`PassMoves`) and its packets
  are made at the picture's time on the CPU, with gif_moves.h's numbers: a colour t of the way, the wobble's texels
  and the view shift's strips t of the way where frame_interp.c's items_pair would pair them, the frame shown before
  by A^t. A TEX0 of the frame drawn is pointed at the buffer the picture is drawn into (frame_interp.c's ad_write),
  and the frame shown before's pass is put between 0xF4 and 0xF5 so the canvas reads the frame shown kept (0xF2).
  `pass_draw_test` puts each pass and the game's own packets for it (written out from game.c and gs_draw.c) through
  the front end: the same register file after, the same pixels with the same colour and texel; and checks the moves.
- **The cost.** A pass that moves in a picture between (a fade, a fill whose alpha ramps, the wobble under water, the
  frame shown before) is moved on the CPU, so that walk is not kept to be drawn again, and the frame's own walk is not
  made its pictures' (counted as "trail paired"). At 009's start after the teleport, 343 of 972 frames (the fill of
  the fade-in). Moving them on the GPU from a record of the pass (as the native GS's `nd_pass.vert` did) would take
  that away; not done.

Checked (1x unless said; on = `--native-draw on`, off = the same kinds but `water` and `passes`, so the packets):
- **The four spots** (000, 009, 009:2, and 099 for "063", which the teleport reads as map 63, a map with no places;
  `--teleport 1900:MAP:PLACE --frame-rate 240`, vblanks 2400 and 2700 with their pictures between): on and off byte
  for byte, the pictures between too. Without `--native-draw` nothing changes (no native draws then).
- **The water maps** (`map_tour.py --maps 8,9,14 --positions 3`, 36 views): on and off byte for byte at 30 Hz, and at
  240 turning (`--turn`) at 1x and 4x in the game's frames. In the pictures between only 014's water differs (views
  4 and 5, up to 28,000 pixels at 1x and 450,000 at 4x, by up to 55 levels): on keeps the reflection where the game's
  frame has it (014 v5's water at x 0-140: 63,77,66 in the frame and on's three pictures between, 46,61,52 in off's,
  which also shows the packet path's dark blocks over the water). Against gs (30 Hz) on is off exactly; 014 v5's
  water matches gs.
- **The passes forced** (KF4_FX at 000, 240, vblanks 2600 and 2700 with their pictures between):
  base, water (the tint and the wobble), drain, drainramp, haze, down and updown (`--flicker-filter`), fill, fade,
  tint, dim, bright40, brightff, shift (`--no-whole-frame`, no pictures between), the pause, the Guardian's Room's
  green fill (035): on and off byte for byte, pictures between too, and the same against gs. prev and aim (the frame
  shown before blended in): their pictures between byte for byte, one game frame of the two 3 pixels by 2 levels
  (one row at x 626-630, the same run to run; see 4x below).
- **At 4x** (`--native-scale 4 --no-native-widescreen`, 240): the four spots byte for byte; water, drain, fade, fill,
  tint byte for byte, pictures between too; prev's pictures between byte for byte, its game frames 76 and 3 pixels by
  2 and 1 levels. The game frame reads the frame shown before, which at 240 is the last picture between drawn into
  that buffer, not dumped. Dumping vblanks 2597-2600 at 1x (so every picture of them is drawn) makes all 16 pictures,
  2600 too, byte for byte: which pictures between of the vblank before are drawn or left out is the difference, not
  the drawing (on moves this pass on the CPU and walks every picture, a little slower).

## Where it stands (2026-10-29)

**Built.**
- The PC renderer (part 1): P1 (hardware textures), P2 (the map's records, objects, items and gold, opaque,
  alpha-tested and blended, kept and drawn by material), P3's three steps (the skinned parts' first pass once, alphas a
  little above 1.0 by Vulkan's blending, the skinned groups on P1's textures), P5's first step (a taken draw's kept
  batches drawn again in its pictures between). P4 and P6 measured and left (nothing to win on the GPU). **P7**
  (2026-10-29, below): the world drawn from its meshes on the GPU, off the GS's batches: the map, objects, items,
  creatures, arm and effects, the material still resolved where the GS's thread meets the draw.
- Smooth play (part 2): the refresh clock (step 0); the pipeline set, the pipeline files off the GS's thread, one
  count of compiles (step 1); a frame recorded once (step 2a); `--frame-rate unlocked` as a rate found as it plays,
  `--frame-rate-cap`, `--tearing` (step 3); shader blending's copies in the render pass in `fast` at every colour depth
  (step 4, item 1).

**What limits the pictures now: the native GS's thread, not the GPU.** `fast` is not a renderer apart from the native
GS: P2 and P3 grew it inside the native GS's scene ("P2, revised" below: a draw's material is only known on that
thread, in the chain's order). Every draw still goes through that thread, and each game frame is recorded there for
its pictures between. At the owner's settings of 2026-10-28 (4x, native widescreen, MSAA 2, `--native-colour high`,
170 Hz; heavier than those part 1 was measured at: MSAA 1, colour exact), map 000:

| | |
|---|---|
| the GPU a picture | 3.33 ms: room for about 300 pictures a second |
| the GS's thread a game frame | about 31 ms of 33 (step 2's measures) |
| a recording, and what the pictures between wait for it | 4.2-5.8 ms; 10.75 ms median in the recording's vblank |
| fixed 240 | 189 pictures a second, 1,353 left out as the GS's thread fell behind (the GPU then busy about 80% of the time) |
| unlocked | settles about 175 a second, as monitor |

Below about 200 a second it keeps up (part 1's "every spot keeps 168-170 at 170 Hz" is still so), which is why part 1
was closed out as done and why it did not show as the limit. Above it, it is the first limit at these settings.

**Since P7** (the table above is from before it): the GS's thread a game frame is 18-19% less at maps 000 and 009
(13.1 and 13.9 ms in `pace_test.py --mode cost` at 240); fixed 240 keeps 239.75 a second at both; unlocked map 000
256-258 a second (was 198-203), map 009 262-264 (was 225-228). The thread is still the limit unlocked, now mostly the
recording and the draws P7 declines ("P7" below: what still goes through the GS's batches).

**Since (2026-10-29, later): two pieces of the recording.**
- **The GS's thread no longer waits for Granite's lock** (RENDERER.md, "The back end"; `KF4_GSN_IMAGES_OFF_THREAD`).
  Sampled with `eu-stack`, 28% of a recording at map 000 was the GS's thread waiting for Granite's device lock, which
  the back end's `next_frame_context` holds while it waits for the GPU: a target dropped by an upload asked for again
  (`destroy_image`), and made again by the next draw (`create_image` asking for a command buffer to make it GENERAL).
  Now the images are let go on the back end's thread and made GENERAL by a call of the stream. A recording at map 000
  (`--fast` at 240, 12,000 vblanks, alternating): 3.85 and 4.11 ms, against 5.42 and 6.76 with the switch off; the
  pictures byte for byte (maps 000 and 009, 204 each, and with `KF4_GSN_BACKEND=0`).
- **P7's calls kept for the pictures between** ("P7, kept", below; `KF4_PC_KEPT`): built, about 3% of a recording at
  000, held back by its table check.

**The owner's decision (2026-10-29, evening): zero native GS in `fast`.** Everything above made the native GS's thread
cheaper; none of it took the native GS out of `fast`. The owner: "I want ZERO native gs in the fast renderer by any
means necessary", the whole job, however long. The plan for it is "The PC renderer: `fast` without the native GS"
(below); it replaces "Next" and "The earlier order" further down.

**Since (2026-10-29, later still): the world's snapshot, steps 1-3 of the next item below** ("The world's snapshot",
below; `KF4_PC_SNAPSHOT`). A picture between makes each world draw the frame kept whole from the frame's snapshot: its
calls put back at its place in the chain, the registers and CLUT left as its writes left them without the writes, its
record made from the kept draw (no fetch, no batches walked). The kept calls' table count is replaced by an exact check
of what each call reads (the table's answer for its texture). At map 000 169,380 of 172,537 kept draws are made so, the
pictures byte for byte as with the switch off and as the build before. **It took less than hoped**: a recording 4.37-4.47
ms against 4.62-4.65 at 000 (-4%), 4.49-4.55 against 4.81-4.94 at 009 (-7%); the world's draws were not most of a
recording any more. Sampled at 000 (106 samples in recordings, `eu-stack`): the snapshot's draws 29% of a recording
(of it the CLUT mirror hashed again after each load about 6%, the table's answer `pcd_table_same` about 7%, the CLUT
loads about 4%), the uploads asked for again (`replay_upload`) 29%, the rest the GS's own batches, the 2D and the
walk. So next is the uploads, then the snapshot's own checks made cheaper (exact still: e.g. the CLUT compared by the
loads' sources, the table's answer kept by its pages), then the GS's own batches, one kind at a time.

**Handoff (2026-10-29, late; branch `pc/renderer`).** Where the PC renderer (`--renderer pc`) stands and what is next,
in order. Read "The PC renderer: `fast` without the native GS" below first, then its "Step 2 built" and "Step 3" sections.
- **Done**: steps 1 and 2 (the whole game from its GIF packets), save states, the pictures between's uploads asked
  for again (0x7C/0x7D), and step 3's native draws of programs 0x000, 0x280, 0x5A0, 0x700 and the GS strips (kinds:
  map, sky, items, picked, objects, skinned, arm, gold, shadows, effects; `pcr_native_kinds()`), each checked against
  the PC renderer on packets at 000, 009, 009:2 and 063.
- **The direction (the owner's decision, 2026-10-29, night)**: the PC renderer is built as a modern game draws, not as
  a second GS emulator ("How it draws", below): the native GS is the exact reference, every picture is compared with
  it, and pc does not need to be exact first. So what comes next is the GPU's way, then the pictures between, then
  what is left of step 3. In order:
  1. **GPU timestamps**: built 2026-10-30 (`pcr_gpu_time.h`; "Item 2 built", below).
  2. **Textures by the GPU's sampler**: built 2026-10-30 ("Item 2 built", below).
  3. **DATE by the stencil**: built 2026-10-30 ("Step 4b's rest built", below), and a blend with no Vulkan form
     the GS's in the shader from a copy of only the rectangle drawn.
  4. **Merged draws**: measured 2026-10-30, not built ("Step 4b's rest built", below): the native batches already
     go out at about 1.2 draw calls a batch, and the draw calls were not where pc's time went: a texture store's
     page walk was, now fixed (pc keeps 60 vblanks a second at 30, was 20). What is left to merge is batches across
     state changes (textures, CLUTs), which is bindless textures; better designed with step 5, whose pictures
     between draw the frame's calls again.
  5. **Step 5, the pictures between on the GPU**: 5a built 2026-10-30 ("Step 5a built", below): pc takes the frame
     and the pictures, walks the frame's packets again for each picture, and the shaders move every vertex; native
     draws are on under pc at 240. **5b built** 2026-10-30 ("Step 5b built", below): a frame's walk kept and drawn
     again for its other pictures while what it read of the texture store is as it was (86% of the pictures at the
     four spots), the pictures byte for byte 5a's. Map 000 at 240: 183-186 pictures a second (5a 101-102); 009 still
     61-66, held by pc's thread taking the packets in (about 68% busy: the frame's own packets, about 200 MB a second
     there, and each frame's first walk), not by the pictures between. **What held 009**, measured 2026-10-30 ("What
     held 009, and resolve's answers kept", below): pc's thread a vblank behind, so every picture between was left
     out; a quarter of its work resolve's walk of the blocks' owners. Resolve's answers kept (exact, by the pages'
     signatures): 009 98-99 pictures a second (was 60), 000 214-226 (155-165 without). **A frame walked once**,
     built 2026-10-30 ("A frame walked once", below): the frame's own walk kept and made its pictures' walk by
     frame_interp.c's rules; 99.9% of frames, 61% of them drawn from it at once (the others had uploaded over
     pages they read before them: their first picture walks the stream). 009 114-119 (100-105 without), 000 at the
     cap (238-240). Sampled again 2026-10-30 ("The store's marks walked by their pages", below): the store's marks
     walked by their pages and an owner's nibbles taken again in place; 009 132-142 (115-118 before). Next: the
     draw calls (merged draws, item 4, designed on the walks kept), then VU1 0x280's records and the uploads.
  6. **Step 3's last two**: `RD_PROGRAM_RIPPLE` (the reflection's water: `gsn_nd_water.inc`, `nd_water.vert`, kind
     `water`) and `RD_PROGRAM_PASS` (the screen passes: `gsn_nd_pass.inc`, `nd_pass.vert`, kind `passes`). As the
     others: the shader's maths into `port/render/shaders/` (`gsn_shaders.h` byte for byte: `cmp` against a copy
     after `tools/build_shaders.py`), the CPU set-up into `port/render/` with a test against the code it stands
     for, the draw in `pcr_native.cpp`, the kind in `kinds()`. `[stats] pc native draws not drawn yet` names what is
     still sent and left out. **Built 2026-11-11** ("Step 3's last two built", above).
  7. The settings not in pc (MSAA, `--native-colour`, aniso, cut-out AA, captures, the overlay's); then step 7
     (`fast` switched to it, the full checks incl. the map tour and pace).
- **How pictures are judged**: against `--renderer native` (the exact GS), not against pc's own earlier pictures;
  the tolerance per case (P1's and P7's were a level or two over few pixels), and every place the GPU's way cannot
  match counted in `--stats`. A rough estimate (not measured): with the GPU's way, several hundred to over a
  thousand pictures a second at the owner's settings; without it, about `fast`'s.
- **How to run things on this machine** (no memory to rely on): every game run under a memory cap (`systemd-run
  --user --scope -p MemoryMax=10G -p MemorySwapMax=0`; the machine has crashed from memory at 240), one at a time;
  `KF4_DATA_DIR` a temporary folder; spots by `--input tests/newgame.pad --teleport 1900:MAP:PLACE --invincible`;
  headless draws nothing without `--dump-frames`; ASan with `out/linux-clang-asan`, `ASAN_OPTIONS=detect_leaks=0:
  halt_on_error=0 UBSAN_OPTIONS=halt_on_error=0`, known reports: SoundDriver_Remote's stack underflow, the decomp's
  and kernel.c's/libmpeg.c's function-pointer casts, Granite render_pass.cpp:52, frame_interp.c:666.

**Next: the world off the native GS, done properly (the owner's decision, 2026-10-29).** Item 3 below is the next
step, not a trim of the recording: the owner chose it over trimming, and chose that it be done the proper way, no
shortcut or workaround (the kept calls' table check, above, is the example: replaced by a check of what is read, not
removed). The kept calls ("P7, kept") are its snapshot of the world. What it has to solve, in order:
1. **Its place in the chain.** The world's draws are among the GS's own (the shadows, the batches P7 declines, the
   effects it does not take, the 2D), and blending and Z are right only in the chain's order. The snapshot's calls go
   back in at their places among the GS's draws, without the walk working them out.
2. **The GS's state they leave.** The GS's batches after a world draw expect the registers it wrote, and the CLUT loads
   its TEX0 writes start. That state is set as each place left it, without the writes (and the CLUT as loaded then).
3. **The picture's records.** Each draw's moves (`nd_model_flags`, the sky's and objects') are made in the walk now, one
   draw at a time. They are made another way: all at once from the kept draws, or on the GPU.
With those, a picture's world costs the GS's thread nearly nothing, and the walk is left with the 2D, the effects the
GS draws and the uploads. Then item 1's rest (the uploads asked for again: 31% of a recording at 000 before the lock
fix) and the GS's own batches, one kind at a time, toward a renderer apart from the native GS.

**The earlier order** (before the owner's decision):
1. **P5 continued: the recording made cheaper** (part 1's P5 and part 2's step 2a are one piece of work). What a
   recording still costs, from both parts' profiles: native draw's draws as a picture between makes them (74% of a
   recording at map 000), of them the skinned models' batches, each batch's state written and flushed again though
   the frame already has it (28%); the uploads a picture between asks for again (13% at map 000, 27% at map 009); the
   test at t 0 of the batches the frame drew nothing of (15% at map 009); the PC world's flushes. One at a time, each
   measured with `pace_test.py` at 240 and unlocked (several alternating runs: a third of runs lose a second with no
   hitch logged, cause not found) and the recording's time in `--stats`. Since P7 the world's batches are draw calls
   made from meshes, so a recording's world is P7's calls again; at map 000 a recording went from 4.3 to 4.8 ms.
   P7's declined kinds next: shader blending for 0x280 and 0x700 by in-pass copies over the plan's screen places,
   the shadows, the water's ripple, the trails.
2. **Then the GPU**, once the thread has room: first what the 0.5 ms of "pass: untagged" is (full-screen work at the
   start of passes, not identified; cheap to find); then step 4's items 2 and 3 (one scene pass a picture, about
   0.15-0.4 ms; the alpha-tested Z-first draws, about 0.1-0.25 ms).
3. **A picture between without the frame's packets.** P5 as first designed interpolated the world from snapshots. The
   snapshot cannot be made on the game's side ("P2, revised"), but the kept draws are a snapshot made on the GS's
   thread: a picture between drawn from them alone (and the GS's recording for the 2D and effects), not by walking
   the frame's packets again, takes the recording's world cost away rather than trimming it. A large change; to be
   decided with step 1's measures in hand.
4. Step 2b (the pictures between made by the back end), if unlocked past the screen's rate still wants it then; and
   step 5 (stalls outside the renderer).

**The owner's choices** (both parts):
- **The name** is `fast` (`--renderer fast`, the Debug page's choice and `settings.json`'s `display.renderer`).
- **The look** is the original's, as exactly as a PC renderer can have it: the PS2's lighting, fog, colours and
  draw order, the vertex programs as they are. Room is left for a remastered look (the shaders' inputs carry what
  per-pixel light and real fog would need), but nothing of it is the default.
- Step 1.4 (smooth play): a pipeline not made yet in `fast`, the batch left out for a few pictures (proposed) or
  the stall. Undecided.
- Unlocked without VRR is mailbox (no tearing; 2026-10-28); immediate is `--tearing`. Whether fixed rates leave the
  settings page's main list is still open.
- P1 under `modern` too: open (part 1's last section).

# The PC renderer: `fast` without the native GS (begun 2026-10-29)

**The goal.** `--renderer fast` runs no native GS code: no `gsn.cpp`, no VRAM image, no GS render targets, no GS
batches, no GS thread. A renderer of its own (`port/gs/pc/`, "the PC renderer") takes the same input the native GS
takes and draws every picture with the GPU's own pipelines. The native GS stays as `--renderer native` (and `modern`),
the reference; paraLLEl-GS stays as `gs`.

**What it takes in.** The seam is the native GS's C interface (`gsn.h`), which `gs.c`, the presenter and
`frame_interp.c` call. The PC renderer has its own (`pcr.h`) with the same calls; `gs.c` picks one at start
(`port/platform/gs_api.h`: `gsr_*`, the native GS's or the PC renderer's). Its input is what the game sends:
- **GIF packets** (PATH1, 2 and 3): register writes, primitives (sprites, triangles, strips), image transfers.
- **Native draws** (register 0x7B, `RD_ND_*`): the game's 3D as draws with meshes, matrices, lights and fog
  (`port/render`, `game/native_draw`), drawn by the PC renderer's own vertex shaders (P7's `pc_000/280/700.vert`).
- **The vblank's display registers**, and the port's marks (0x7C uploads asked again, 0x7D pictures between).

**What it keeps instead of VRAM.**
- **Surfaces**: every buffer the game draws into (the frame and Z buffers, the reflection's 256 x 256, the frame copies,
  the scrolling textures' targets) is a GPU image, found by its base, width and format; at the renderer's scale, its own
  formats (D32F depth).
- **Textures from their uploads**: every image transfer is kept as a chunk (where it went, its bytes, a hash), and an
  ownership map says which chunk owns each block. A texture is decoded from the chunks that own it (the PS2's texel
  formats are the game's data: decoded with the swizzle functions of `gsn_vram.c`, a format library, the only file of
  the native GS's directory the PC renderer uses), or read from the surface drawn there since.
- **Copies** (local to local) are image copies between surfaces; read-backs (local to host) read a surface.

**How it draws: as a modern game draws, not as an emulator (the owner's decision, 2026-10-29, night).** The native
GS is the exact GS, and every picture is compared with it: the PC renderer does not need to be exact itself, and is
not built exact first. Each piece is built the GPU's own way from the start and judged against the native GS's
pictures, with the same comparisons (`--dump-frames` at the spots, the map tour), the tolerance decided case by case
as P1 and P7 were accepted in `fast` (a level or two over a few pixels). An exact path is kept only where the GPU's
way measurably cannot match, each such case counted in `--stats` so that they can be worked off. What that means:
- **Textures**: decoded once (the CLUT applied) into ordinary GPU textures and read by the GPU's sampler, as P1 did
  in `fast`; not the GS's texel reads, filtering and CLUT lookups in the pixel shader. The exact read only for what
  the sampler cannot stand in for (measured: e.g. a texture read from a surface the game drew, a region clamp).
- **Colour, fog, alpha test**: plain shader maths; blending by Vulkan's blend. Where a blend has no Vulkan form, a
  copy of only the rectangle drawn, not the surface.
- **DATE** (the destination alpha test): the stencil, not a copy of the surface.
- **Draws**: batches of the same state merged into few calls (instanced or appended), not one call a batch.
- **Native draws** by their records (P7's maths, the material from the registers at the draw).
- **Pictures between**: the frame drawn once; each picture between the frame's calls again on the GPU with the
  vertices moved, drawing again only what moves.
- **Measured from the start**: GPU timestamps in the PC renderer, so that each change's cost is seen.
The display registers pick the surface shown; the picture goes to the presenter as the native GS's does.

(Until 2026-10-29 the PC renderer was built the other way: `pcr_prim.frag` is the GS's pixel again, texel reads,
bilinear by hand, CLUT, DATE by a copy of the surface, one draw call a batch. That is what the steps below replace;
the surfaces, the texture store, the uploads, the native draws' records and VU1's shader maths stay as they are.)

**The order** (each step runs, and is checked against `--renderer native`'s pictures; steps 1-3 built, as far as
the handoff above says, the GPU's way coming next):
1. **The skeleton**: `--renderer pc` (development name; `fast` is switched to it at the end), the device, its thread,
   the vblank, the presenter, a picture (cleared surfaces), `gs_api.h` in `gs.c`, the presenter and frame_interp.c.
2. **The 2D**: register state, sprites and triangles from GIF packets, uploads and the texture store, surfaces, the
   scanout. The title, menus, HUD, messages and movies show.
3. **The 3D**: native draws (0x000, 0x280, 0x700) with depth, fog, alpha test, blending, layers.
4. **The rest of the play**: shadows (0x5A0, DATE as the stencil), the PATH2 strips (the trail, the water), the
   reflection target, the frame copies, the scrolling textures, the screen passes.
4b. **The GPU's way** (the decision above): GPU timestamps; textures by the sampler; DATE by the stencil; merged
   draws; a blend's copy of the rectangle drawn only. Before step 5, so that the pictures between are built on it.
5. **Pictures between**: each picture drawn from the frame's draws with the picture's camera and moves, on the GPU.
6. **The rest of the interface**: save states, captures, `--dump-frames`, `--stats`, the overlay's settings.
7. **`fast` switched to it**, the checks (pictures against native at every spot and the map tour's maps, sanitizers,
   speed), the old `fast` kept as `--renderer modern`.

## The pictures between on a thread of their own: the pump (built 2026-11-06)

docs/port/PERFORMANCE.md's item 1. At 009 pc's thread was 97% busy: 45% drawing the pictures between again
(`draw_again`), 41% taking the frame's packets in.

**What it does** (pcr.cpp's `Pump`; `KF4_PC_PUMP=0`: no pump, as before):
- A second thread, `kf4-pc-pump`, with a canvas, a `Between` and GPU times of its own. kf4-pc still decides each
  picture between as before (`fits`, or walk the stream); a picture it would draw again from the kept walk it hands to
  the pump instead, with, the first time for that walk, the frame (`PumpFrame`: the walk, `Between::hand_over`'s
  frame as walked, and `Canvas::copy_surfaces`: every colour and depth surface copied on the GPU, the kept frame shown).
- The pump's canvas draws the walk on those copies (`Canvas::draw_again_alone`): the draws, uploads and copies only
  as far as they fall on its surfaces, none of the texture store's changes (CLUT loads passed over): what the draws
  read of the store is held in the walk. So kf4-pc's store and surfaces now see only the game's packets and the walks
  of the stream, not each picture between's side effects.
- The picture that shows a pumped picture between (the extra one, or the vblank's own at phase 0) is the pump's too.
  kf4-pc's own pictures are handed out after the pump's before them (sent through the pump while it has work; one whose
  sender waits, or whose pixels are kf4-pc's own, waits for the pump), so the presenter, the ring and
  `pcr_backlog` (now vblanks sent less vblanks handed out) see every picture in order.
- Granite's frame contexts are moved on by kf4-pc only; that waits for every thread's open command buffer, so the
  pump submits every 128 batches and at each picture's end (a pump left open blocked kf4-pc for good).
- `[stats] pc pump` line; the pump's GPU time is added into the one `GPU time` line.

**Checks.** Against `KF4_PC_PUMP=0` at 000, 009, 009:2, 063 at 240 (`KF4_INTERP_DRAW_ALL=1`, vblanks 2300-2650
every 7th, 204 pictures a spot): 000 byte for byte; 009 and 063 differ only in the top two rows of the picture (half
a GS line at 4x), where both builds are equally far from the native GS (009 mean 39.4 against 39.7 off the native GS's
pictures in those rows; 063 16.4 against 16.1); 009:2 from vblank 2447 in 2 pixels by 1. The rows are the buffer's
leftover top line, which the pictures between no longer draw over in kf4-pc's buffers. ctest 14/14. ASan + UBSan pc
run at 009:2: only the known reports. TSan pc run at 009: of 266 reports, 4 in renderer code, all inside RADV while
the two canvases are made at once at start (Mesa's futex locks are not seen by TSan); the rest the known dmac, kernel
and menu ones. Not run: native and `fast` checks (their paths are not changed), clang-tidy (no configuration).

**Speed** (`pace_test --hz 240 --frame-rate 240 -- --renderer pc`, 009, runs alternated): with the pump 147.6,
161.4, 157.9 pictures a second (lowest seconds 112, 134, 138); without 138.3, 133.6, 132.3 (123, 126, 118). An
earlier set of runs at 009 and 000 was too noisy to read (both builds fell to 82-85 in one run). GPU time a picture
2.7-3.2 ms with the pump against 2.5 without; fewer submits (one a picture) gave 3.0 against 3.2, so most of it is not
the passes. Not twice as many: sampled at 009, kf4-pc is now 44% and the pump 61% waiting for work, gs.c cut no
picture and left none out, so what holds 009 now is outside pc's two threads (to measure next: the GPU's time and
frame contexts, the presenter's pacing).

**Corrected (2026-11-06, docs/port/PERFORMANCE.md "Measured again after the pump").** gs.c did leave pictures out:
every second vblank's three, as kf4-pc's vblank picture came a third of a millisecond after the next vblank was
posted (gs.c's count was not printed with `--renderer pc`). And the pump's pictures were not on the GPU's queue when
handed out (Granite holds a plain submit until the next frame context, which only kf4-pc moves on): a median 17 ms
later. Both fixed (`gsr_behind`, `submit_now`, `Canvas::submit` flushing): 009 at 240 239.3-239.7 pictures a second.

**Corrected (2026-11-07): the pump leaked GPU memory above about 250 pictures a second.** Granite frees what a
thread lets go of only as its frame contexts come round, and kf4-pc moved them on only at the pictures it made; where
the pump made most pictures (`--hz 500`), its blocks and command buffers were never freed (6 MB a picture) and filled
the GPU's memory and the system's in seconds. kf4-pc now moves the frame context on at the pump's pictures too, and
waits before sending a frame while one is not yet started. Details and numbers: docs/port/PERFORMANCE.md "Built
2026-11-07"; 009 uncapped 352-364 pictures a second, 240 now with 1-2 uneven refreshes a second (was 25-32).

## The store's marks walked by their pages (built 2026-10-30)

**Measured first** (009 at 240, `--renderer pc`, the owner's settings, 250 samples of `kf4-pc` with `eu-stack`): pc's
thread 92% busy (4% waiting for the game, 4% for the GPU). 48% drawing the pictures between again (`draw_again`): of
it 16% Vulkan's draw calls, 13% `TextureStore::drawn`, 8% the uploads asked again (`own`), 5% CLUT loads. 39% taking
the frame's packets in: 17% program 0x280 (its batches' draw calls 7%, its CLUT loads 4%, `vu1_kicks_280` 3%,
`vu1_record_280` 2%), 9% uploads (`image_data`). Over both, Granite's draw 26%: RADV's `CmdDraw` 11%, the descriptor
sets 7%, pipeline binds 6%.

**What it does** (`pcr_textures.cpp`; exact: the store's answers, signatures and seqs are as before):
- `TextureStore::drawn` walks only the pages the rectangle is in (`each_page`, which `pages_of` is now built on),
  not all 512 testing a bit set each (the 13% above: a picture drawn again marks every batch's rectangle).
- `take_nibbles`: a block's newest owner taking the same nibbles again (an upload found again, an upload asked again)
  only has its seq moved. The full walk took its entry out (the older owners hold none of those nibbles) and put it
  back newest; the chunk's count dropped to 0 only for that moment, and `mark_unowned` leaves alone a chunk that owns.

**Checks.** Byte for byte against dc18e2f6 at 000, 009, 009:2 and 063 at 240 (`KF4_INTERP_DRAW_ALL=1`, vblanks
2300-2650 every 7th: 204 pictures a spot). ctest 13/13; `pcr_textures_test` under ASan + UBSan. Not run: an ASan pc run
in play, TSan, native and `fast` checks (their paths are not changed), clang-tidy (no configuration).

**Speed** (`pace_test --hz 240 --frame-rate 240 -- --renderer pc`, three runs a side alternated): 009 132.1-142.1
pictures a second (dc18e2f6 115.1-118.0); 000 237.1-239.8 (234.1-239.8, the cap). The GPU 2.5-2.8 ms a picture.

**What is left** (from the samples above): the draw calls (fewer of them: the merged draws of item 4, now with step
5's walks kept to design them on), program 0x280's records and kicks, the CLUT loads (a key vector made for each, 512
mirror writes), the uploads.

## A frame walked once (built 2026-10-30)

**Why.** Sampled at 009 at 240 (250 samples of `kf4-pc`): 38% of pc's thread was each frame's first picture between
walking the frame's stream (frame_interp.c's copy of the frame's packets), 37% the frame's own packets, 20% the other
pictures drawn again (5b); the thread was busy all the time, 101 pictures a second. The stream is the frame's packets
again, changed by replay_pending: FRAME, TEX0 and BITBLTBUF naming the buffer drawn name the buffer shown, the
uploads into anything else asked for again (0x7C), read-backs, copies into anything but the frame, SIGNAL, FINISH and
LABEL left out, 0xF0 and 0xF1 around it, the records that set TEX0 to the frame shown between 0xF4 and 0xF5. So
the frame's own walk, kept, can be made the stream's walk by those rules, and the frame is walked once.

**What it does.**
- **The stretch** (pcr.cpp's Intake): the packets from the first after a vblank to the next vblank are kept as they
  are walked (the canvas's `keep_walk`, the kicks numbered as a picture's walk numbers them, the native draws' records
  made twice). A picture between, a frame taken or a state loaded in a stretch drops it (the 0xF2 mark after the
  vblank), and frame_interp.c's replays sent as packets (the exact restore, between 0xF0 and 0xF1) are kept out.
  Stretches are kept only while frames are taken (none at 30, in the menus).
- **The frame's account** (`GsnInterpFrame`, gsn.h, filled by frame_interp.c's gpu_frame; the native GS reads none of
  it): the frame's packets' bytes, the buffer drawn and the one shown, whether uploads are asked for again, whether
  the frame shown is kept. A stretch whose bytes are not the frame's, or whose kicks are not its count, is not it.
- **The rules** (`pcr_frame_walk.h`, plain functions, `pcr_frame_walk_test`): a draw's FRAME and TEX0 moved to the
  buffer shown; an upload into anything but the buffer drawn asked for again (its chunk the frame's), a read-back and
  a copy into anything but the frame left out; a textured draw reading the frame shown between 0xF4 and 0xF5. Refused
  (the frame walks its stream, counted): a copy from or into the buffer drawn, an upload into it, a draw textured from
  it other than from its surface's pixels (a full-screen pass that reads the frame it draws: looked up again in the
  buffer shown, as the stream has it), a trail its pictures move on the CPU, a native batch without its picture's
  record, a mark or upload asked again of a picture's. The stream's own choices for those (frame_interp.c's table of
  copies) are not handed over: none was met at the four spots.
- **`Canvas::as_picture_walk`** makes the kept walk from the stretch's: the batches' FRAME and TEX0 moved, a GIF
  batch's numbered corners moved (`Batch::numbered`: the corners name their kicks in the stretch, which nothing moves
  outside a picture), a native batch's runs drawn by their pictures' records, the uploads as uploads asked again, 0xF0
  before and 0xF1 after. Then the frame's pictures are drawn again from it as from a 5b walk, while what it read of the
  store is as it was (`fits`); its kicks are the frame's walked kicks (`Between::adopt_live_kicks`).
- **Native draws** (`NativeDraws::begin_frame_kept`): in the stretch each draw's record is made twice side by side, the
  frame's and the picture's (its move: `PartMoves::draw` with `PartWalk::frame_kept` keeps the place and gives the
  move a picture of the frame gives; `part_moves_test` checks it against a frame's walk and a picture's).
- A read-back in a kept walk no longer spoils it (the stream never had one); `KF4_PC_LIVE_WALK=0`: the frame's stream
  walked for its first picture, as before. `[stats] pc pictures between` counts the frames made so and why not.

**Checks.** At 000, 009, 009:2 and 063 at 240 (`KF4_INTERP_DRAW_ALL=1`, vblanks 2300-2650 every 7th, fixed clock):
byte for byte with `KF4_PC_LIVE_WALK=0` (204 pictures a spot). 947-949 of 948-950 frames a spot made from their own
walk (the first frame of a run: not the frame), none refused by the rules; 366-370 a spot then walked their stream,
the store changed (the frame had uploaded over pages it read before the upload, so its own walk read what the stream's
would not), so 6,262-6,279 of 6,632-6,646 pictures drawn again (5,684 before). ctest 13/13; pcr_gif_test,
pcr_frame_walk_test, part_moves_test under ASan + UBSan; an ASan + UBSan pc run at 009:2 at 240: the known reports
only. Not run: native and fast checks (frame_interp.c only fills fields the native GS does not read), TSan, clang-tidy
(no configuration).

**Speed** (`pace_test` at 240, `--renderer pc`, two runs a side alternated): 009 113.9-118.6 pictures a second
(100.1-105.4 with `KF4_PC_LIVE_WALK=0`); 000 238.3-239.8 (238.1-238.9: at the cap either way; 178-226 measured on other
days). The GPU 2.3-2.6 ms a picture (2.2-2.3). Less than the first walks' 38%: 39% of frames still walk their
stream, and keeping the stretch (its batches copied, its kicks, two records a draw) costs the frame's own walk.

## What held 009, and resolve's answers kept (built 2026-10-30)

**What held 009** (measured at 240, `--renderer pc`, the owner's settings): in play no picture between was shown at
all, 60 pictures a second, each a vblank's own. pc's thread was a vblank behind as each vblank began (`pcr_backlog`
1), so gs.c's cut came down to one picture between a vblank and left that one out every vblank too (in a diagnostic
build; gs.c's "left out as the renderer was behind" line is printed only under the native GS). pc's thread, sampled
208 times with `eu-stack`: 23% waiting for work, 9% waiting for a GPU frame context, the rest the frame's own packets
(no picture between was walked). Of that work about a quarter was `TextureStore::resolve` walking every block's
owners (`owners_of`, its `Cover` compares), about 15% VU1 0x280's records and kicks, about 14% uploads found again.
In a diagnostic build 95.8% of resolves asked what was answered before while every page that answer read had its
signature (5b's) as then, and none of those answers differed from the walk's.

**What it does.** `TextureStore` keeps resolve's answers (`pcr_textures.h`'s top): under what was asked (the
texture's shape, its levels' places and the part read: the key without its chunks), the walk's source, missing
block and chunks, the blocks it read (a 32-bit mask a page) and their pages' signatures then. The same question
while every one of those pages answers as it did is answered from it without the walk; the chunks are sorted by
their uploads as now, so the key and the decode found are the walk's. While a stretch's reads are watched (5b), a
kept answer notes the blocks its walk read, as the walk would. Each page's signature is kept and made again only
after the page changed (an owner taken, a draw or copy over it, the CLUT buffer written). `answers_kept` (8,192)
bounds them, all let go when met. `KF4_PC_RESOLVE_KEPT=0`: none kept, every resolve walks.
- **The signature now has the page's draw marks** (whether it was ever drawn into, low parts and top byte apart):
  a block no upload owns answers DrawnSurface or NotUploaded by them, which the signature did not show before (so 5b's
  check could take a page drawn into for the first time as unchanged). The replays at the four spots are as before
  (5,684 of 6,632 drawn again at 000 and 009).
- `[stats] pc texture store` says how many resolves were answered kept.
- Tests: `pcr_textures_test` runs every random call on a second store that keeps nothing, and every answer, CLUT
  load, read-back and watched stretch must be its; `kept_answers` (asked again, an upload elsewhere, the same chunk
  again, a different upload into the page, a draw over it, a DrawnSurface answer asked again and then uploaded over,
  a page never uploaded then drawn into, watched, forget). Without the draw marks in the signature both fail.

**Checks.** At 000, 009, 009:2 and 063 at 240 (`KF4_INTERP_DRAW_ALL=1`, vblanks 2300-2650 every 7th): byte for byte
with `KF4_PC_RESOLVE_KEPT=0` (204 pictures a spot; 162 at 009:2, the same in both runs: some of its dump vblanks have no
pictures between). Answered kept: 95% of resolves at 000, 97% at 009, 94% at 009:2. ctest; `pcr_textures_test` under
ASan + UBSan; an ASan + UBSan pc run at 009:2 at 240 (only the known reports). No native or `fast` checks: their
paths are not changed.

**Speed** (`pace_test` at 240, `--renderer pc`, runs alternated): 009 98-99 pictures a second (60 with
`KF4_PC_RESOLVE_KEPT=0`: none between), 000 214-226 (155-165 with it off; the build before 163-168 that day, 5b's
183-186 measured another day). The GPU 2.1-2.3 ms a picture.

## Step 5b built (2026-10-30): a frame's walk kept, and drawn again for its other pictures

**What it does.** The first picture between of a frame walks the frame's packets as in 5a, and the canvas keeps what
the walk made (`pcr_canvas.h`'s `KeptWalk`): its batches (state, vertices, textures, native runs, with the images and
buffers they use held), and in their order its uploads, uploads asked for again, copies, CLUT loads and marks. The
frame's other pictures make those again (`Canvas::draw_again`) with only their Picture buffer new
(`Between::begin_picture_again`; the kicks are the walk's): no front end, no native draws, no texture store resolves.
A texture read from a surface is copied from it again, DATE and shader blending are set up again, and the store's own
changes (uploads again, pages drawn, copies, CLUT loads) are made again, so that the store and the surfaces are left
as a walk leaves them. `KF4_PC_KEPT_WALK=0`: every picture walks (5a).
- **Every walk of a frame starts from one state**: the front end's when the frame was handed over
  (`GifFrontEnd::State`, `state()`, `set_state()`), and the live state is put back after the walk (as the native GS's
  interp_capture; pc walked from what the live packets left, and left the walk's registers behind). So every walk of
  a frame makes the same kicks.
- **When a kept walk is drawn again**: while every page of the texture store it read from before it (a block read
  before the walk wrote it; page 512 the CLUT buffer) answers as it did then (`TextureStore::watch_reads`,
  `reads_watched`, `unchanged`; `page_signature`: a hash of the page's owners, their nibbles and whether each is
  drawn over, never the upload counts, so a walk that uploads a chunk again and draws over it again leaves its pages'
  signatures as they were). Change counts were tried first and turned every picture away: each walk's own uploads
  again and draws moved them, though the pages ended answering what they did. Surfaces are looked up again as the
  kept draws are made, so they need no check.
- **Not kept** (the pictures walk, counted): a walk that read back from the GS's memory, or whose kicks were not the
  frame's count, or that met a trail paired with the frame before (its points are moved t of the way on the CPU,
  `NativeDraws::moved_on_cpu`; moving that into the shader is to come).
- `[stats] pc pictures between` now says how many were drawn again, and why the others walked.
- The texture store's chunk ids are no longer used again after a state is loaded (`forget`), so that a signature
  cannot match a new chunk (the random seeds' counts move by one or two uploads found again).
- Tests: `pcr_gif_test`'s `test_state_round_trip`; `pcr_textures_test`'s signatures (an upload again of the owner, a
  draw over it again, a different upload, an equal and a different CLUT load) and the watch (read before write, a
  page partly written in the stretch, a stretch uploading again over owned blocks unchanged after).

**Checks.** At 000, 009, 009:2 and 063 at 240 (`KF4_INTERP_DRAW_ALL=1`, vblanks 2300-2650 every 7th, 204 pictures a
spot): with the kept walks byte for byte as with `KF4_PC_KEPT_WALK=0`, and that byte for byte as the build before
(5a): the walk's start state changed nothing at these spots. In each run 5,684 of 6,632 pictures (5,696 of 6,646 at
063) were drawn again, the rest each frame's first; none turned away by the store, none not kept. So the pictures
against the native GS are 5a's (the table above).

**Speed** (`pace_test` at 240, `--renderer pc`, two runs a side alternated): 000 183.4-186.1 pictures a second (5a
100.9-101.7), the GPU 2.13-2.19 ms a picture (1.90-2.04); 009 61.1-66.2 (63.5-64.9). At 009 pc's thread is about
68% busy, sampled all in taking packets in (uploads, kicks, CLUT loads: the frame's own packets and each frame's first
walk, inlined together), and only about 60 pictures a second are asked of it: what holds 009 is to be measured.

## Step 5a built (2026-10-30): the pictures between drawn by pc, moved on the GPU

**What it does.** frame_interp.c's GPU way ("On the GPU") now runs under pc too (`port_gs_interp_gpu`; `KF4_PC_BETWEEN=0`
turns it off: the CPU way and the game's packets, as before). pc keeps the frame (`pcr_between.h`, `Between`); for each
picture it walks the frame's packets again through its front end, which numbers every vertex kick fed (XYZ2, XYZF2,
XYZ3, XYZF3; PACKED, A+D or REGLIST) as frame_interp.c does, and the stretch in which ST, RGBAQ (and its Q) and UV were
last written (the owners: gsn.cpp's `InterpKick`). Each numbered kick goes into a buffer of the picture's own; the
picture's cameras, time and runs into another (`gs_replay.h`'s Picture); the frame's moves into a third. Set 1 of
every canvas draw is those three (zeros outside a picture).
- **GIF primitives**: a corner names its kicks (`pcr_prim.vert`'s `a_kicks`: a triangle's own, its colour's, its third;
  a sprite's two and which corner) and is made again of them at the picture's time in the vertex shader, by
  `gs_replay.comp`'s maths, now in `port/render/shaders/gif_moves.h` (`moved()`, shared: `gsn_shaders.h` byte for
  byte). The kick's registers as the picture has them (`kick_now`) is there for pc only (`GIF_KICK_NOW`): as a function
  it changed the native GS's SPIR-V, so `gs_replay.comp`'s main keeps its own copy of it. A triangle whose third kick, or
  one of the two before it with a place, is dropped is not drawn. A batch with moved corners is taken to draw anywhere
  in its scissor within its surface (the store's pages drawn, DATE's and shader blending's rectangles).
- **Native draws**: their records carry how they move (`port/render/part_moves.hpp`, `PartMoves`: the native GS's
  `nd_mark`, `nd_key`, `nd_remember`, `nd_model_flags`, `nd_between`, the same maths; the native GS keeps its own copy),
  and `pcr_nd000/280/700.vert` move each corner by `vu1_*_maths.h`'s `moved()` as the native GS's `nd_*.vert` do. The
  trail's points are written t of the way from their partners in the frame before (`PartMoves::strip`, nd_strip.vert's
  rounding) on the CPU, as the packets are walked anyway; not seen paired at the four spots (no swing in them).
- `port_gs_native_draws_now` is true under pc whenever the pictures between go the GPU's way (`port_interp_on_gpu`:
  not `KF4_INTERP_CPU`, not `KF4_INTERP_STILL`).
- Tests: `part_moves_test` (a part's move against its matrices at t 0, 1 and between, the moves refused, the sections,
  the pairing by identity, the trail's pairs); `pcr_gif_test`'s `test_kick_numbers`.

**Pictures against the native GS** (240, `KF4_INTERP_DRAW_ALL=1`, vblanks 2300-2650 every 7th; pixels over 8 levels a
picture): the pictures between are as close as pc's own frames at every time.

| | frame | `_250` | `_500` | `_750` |
|---|---|---|---|---|
| 000 | 424,509 | 424,564 | 424,560 | 424,532 |
| 009 | 834,221 | 835,805 | 835,762 | 835,625 |
| 009:2 | 435,821 | 435,999 | 435,904 | 436,166 |
| 063 | 483,414 | 483,415 | 483,415 | 483,416 |

In every run 968 frames and 6,772 pictures were taken, none of another frame, none whose kicks were not the frame's.
Seen at 240 now that native draws are on there, but there before at 30 as well (the build before, map 000): pc's top
row of the picture is grey (row 0 about 155, the rows below about 87; native 54), a native draws' matter, not looked into.

**Speed** (`pace_test` at 240, `--renderer pc`, two runs a side alternated): pictures a second at 000 99.6-103.1 (the
build before 62.8-64.4), at 009 61.2-63.8 (60.7-60.8); the GPU a picture 1.85-2.05 ms (1.33-1.41: the native batches
are now drawn in every picture, moved). What limits it is pc's thread: walking the frame's packets again for every
picture (5b, the handoff's item 5).

**Checks**: `KF4_PC_BETWEEN=0` byte for byte with the build before at the four spots (204 pictures each); native and
`fast` byte for byte with the build before at the four spots; the map tour as before (450,157 pixels, worst 106 at map
030, every scanout the same); ctest, `vu1_test`; ASan + UBSan pc runs at 000 and 009:2 at 240 (only the known reports).

## Step 4b's rest built (2026-10-30): DATE by the stencil, shader blending from a rectangle, pc's thread freed

**What limited pc was its own thread, and not its draw calls.** Sampled at map 000 (30, native draws; 160 samples of
`kf4-pc` with `eu-stack`): 76% of the thread was `TextureStore::drawn`, which after each batch marked the pages it
drew over by walking every 8 x 8 block of its rectangle through `gsn_vram_offset` (a native batch's rectangle is its
scissor: the whole screen, twice with Z, about 100 batches a picture). `pages_of` (`pcr_textures.h`) now works out the
pages of a rectangle on a page's start (every FRAME and ZBUF) by rows and columns of pages, as `gsn_vram.c`'s
`pixel_nibble` has them, and walks the blocks only for other bases; `pcr_textures_test` checks it against every pixel's
address for every format, Z formats too (3,000 rectangles a seed, wrapping and odd widths). After it, the same thread
waits 59% of the time.

| `pace_test`, 15 s, two runs a side alternated | the build before | now |
|---|---|---|
| at 30, native draws: game vblanks a second, maps 000 / 009 | 21.2-21.3 / 19.9-20.4 | 59.93 / 59.93 |
| at 240 (packets): pictures a second, 000 / 009 | 61.4-62.0 / 60.7-60.8 | 61.4-63.6 / 60.9 |
| GPU ms a picture at 240, 000 / 009 | 1.50-1.51 / 1.51-1.53 | 1.43-1.44 / 1.46-1.47 |

At 240 pc is still held by the packets of its pictures between (step 5); at 30 the GPU's time is within the runs'
noise (1.18-1.46 ms against 1.24-1.46).

**DATE by the stencil.** Depth surfaces are D32F with a stencil (D24S8 where the device has no D32FS8; neither: DATE
by a copy). A DATE batch's pass has its Z buffer's surface (ZBUF's, ZTE on or off) and reads its colour target as an
input attachment of itself. Before the batch, over the rectangle it draws in, the stencil is cleared and set where
the target's alpha bit 7 is DATM (`pcr_date.frag`); the batch draws where it is set. A pass that writes alpha with FBA
and DATM 0 clears the stencil where it draws, so a later primitive of the batch no longer draws there, as on the GS
(the copy let overlapping shadow triangles darken twice). A pass that writes alpha without FBA writes a bit 7 the
stencil cannot know: kept, and counted (`[stats] pc canvas, not done yet: ... writing an alpha the stencil does not
know`: at 000, 603 of 1,080 DATE draws, a 2D batch of DATM 1 sprites; the copy was no more exact there).
`KF4_PC_STENCIL=0`: DATE by a copy of the surface, as before. The GPU's "DATE copy" (0.04 ms a picture) is gone.

**A blend with no Vulkan form** is the GS's `(A - B) * C >> 7 + D` in `pcr_prim.frag` (as `gs_frag.h`'s: COLCLAMP,
PABE, a 24-bit frame's Ad 0x80), Vulkan's blend off, reading the destination from a copy of only the batch's
rectangle (GPU work "blend copy"). A GIF batch is split where a primitive's pixels overlap those of the batch's
primitives before it, so each reads the destination as the GS would; a native batch's triangles cannot be split on the
CPU and are counted (`... of them native: their triangles may overlap`). None of the four spots has such a blend (0
counted before and after); checked by sending every blended GIF batch through it (a diagnostic build, native draws
off): within 3 levels of Vulkan's blend at 000, 009:2 and 063; at 009, 2,269 pixels over 8, all in the picture's top
row, where both differ from the native GS by about 215 levels anyway. `KF4_PC_SHADER_BLEND=0`: the nearest Vulkan
blend, as before (counted).

**Pictures against the native GS** (vblank 2700, 30, native draws; pixels over 8 levels): 000 432,166 -> 428,442; 009
868,994 (the same); 009:2 464,453 -> 458,533; 063 483,639 (the same). With `KF4_PC_STENCIL=0 KF4_PC_SHADER_BLEND=0`
byte for byte with the build before (204 pictures a spot at 240, 51 at 30).

**Merged draws, measured.** At 000 (3,397 vblanks) 1,974,468 native batches went out in 398,504 draw calls (`--stats`'
`pc canvas` line now has them) and 332,623 canvas batches in all: about 117 calls a vblank. Growing runs across draws
would save at most the calls past one a batch (about 66,000, 20 a vblank); `CommandBuffer::draw` was 3 of 160 samples.
Not built. What breaks batches is state (texture, CLUT, registers), so merging further is bindless textures and the
material in the draw's record; it pays when step 5 draws the frame's calls again for each picture between, and is to
be designed with it.

## Item 2 built (2026-10-30): GPU timestamps, and textures by the GPU's sampler

**GPU timestamps** (`port/gs/pc/pcr_gpu_time.h`): a timestamp wherever the kind of work recorded changes (the 2D,
native batches by program, DATE's copy, a texture's copy from a surface, surfaces, uploads, copies, the picture), each
interval added to its kind; on with `KF4_GSN_GPU_TIME=1` (pace_test sets it) and the Shift+P profiler. `--stats`
prints `[stats] pc renderer GPU time: ...`, pace_test's "GPU ms a picture". The time between submits is not counted.
Run time the same with it on and off (map 000 at 30, 3,100 vblanks: 57.8 and 58.8 s).

**Textures by the sampler.** A texture from uploads is decoded once with the CLUT and TEXA applied
(`colour_texels` in `pcr_textures.h`, tested in `pcr_textures_test`) into an RGBA8 UNORM image, found again by its
uploads, TEXA and CLUT colours compared whole, and read by `textureLod` with the GS's level from Q; MMAG and MMIN are the
sampler's filters (without mipmaps the lod is held at 0.25, which reads level 0 by MMIN's filter), REPEAT and CLAMP
its address modes. The GS's read in `pcr_prim.frag` stays for the region modes and textures decoded in part (counted:
`[stats] pc canvas textures`), and for textures read from surfaces. `KF4_PC_SAMPLER=0` reads every texture the GS's
way. At maps 000, 009, 009:2 and 063 every textured batch was the sampler's (0 by the GS's read).

**Measured** (pace_test, 4x, native widescreen, 15 s, two runs each side alternated; MSAA, aniso and colour are not in
pc yet):

| GPU ms a picture | GS's read | sampler |
|---|---|---|
| 240 (all packets), maps 000 / 009 | 1.82-1.83 / 1.87-1.88 | 1.31-1.36 / 1.34-1.41 |
| 30 (native draws), maps 000 / 009 | 1.07 / 1.21-1.22 | 0.86-1.00 / 0.95-1.08 |

At 240 the 2D (GIF primitives) went from 1.60-1.66 to 1.09-1.18 ms a picture. Pictures against the native GS at vblank
2700 (at 30, native draws on), pixels over 8 levels: 000 441,724 -> 432,166; 009 906,109 -> 868,994; 009:2 502,334 ->
464,453; 063 505,549 -> 483,639 (closer at every spot; pc and native differ this much before the change too, not yet
looked into). Against the GS's read: 000 and 063 within 7 levels; the fire at 009 13,325 pixels over 8 (worst 93), its
additive layers adding up the sampler's finer filter fraction (8 bits against the GS's 4).

**What limits pc is not the GPU.** Pictures a second did not move: 61 at 240 (packets: the game's VU1 runs and GIF,
step 5), and at 30 the game itself runs at 19-21 vblanks a second under pc with native draws (2.1-2.9M native batches
in 3,196 pictures, one at a time on pc's thread), already before this change. With about 1 ms of GPU a picture, the
next gains in pictures a second are on the CPU: merged draws (item 4) for the native batches, and step 5.

## Step 2 built (2026-10-29): the PC renderer draws the whole game from its GIF packets

`--renderer pc` draws every picture the game sends as GIF packets, with none of the native GS's code
(`port/gs/pc/pcr_canvas.cpp`, shaders in `port/gs/pc/shaders/`, built by `tools/build_shaders.py` into
`pcr_shaders.h`). Native draws are turned off under it for now (the game's VU1 packets carry the 3D), so the picture is
whole; drawing the native draws themselves is step 3.

- **Surfaces**: a GPU image for each buffer drawn into (base page, width, format), as tall as what is drawn in it. A
  frame whose base is inside a surface of its width is that surface at an offset (the game's 128-wide targets at 1d2
  and 1d4 are one buffer a page row apart); a surface made takes in those of its width it overlaps. A new surface
  starts as the uploads left its pages.
- **Textures**: the texture store's (uploads, exact, `pcr_textures_test`), or a copy of the surface drawn there,
  read at the surface's scale (its own pixels, filtered among them; the wrap modes on the GS's texels): read one
  pixel a GS texel, the game's full-screen pass over its frame cut a 4x picture to 640x448's detail (until
  2026-11-07). Local copies of uploads' pixels are uploads of what they write (the scrolling textures); read-backs (local to
  host) are made from the uploads or the surface: the game reads its effect atlas back and uploads it again, so a
  read-back of zeroes lost the torches' flames.
- **The GS's pixel** in `pcr_prim.frag`: texture read (wrap modes, filters, the mipmap level from Q, CLUT, TEXA),
  texture function, fog, alpha test with AFAIL's passes, DATE (from a copy of the destination), FBA; ALPHA by
  Vulkan's blend (the colour premultiplied in the shader where it needs it; As above 128 is clamped: counted).
- **Pictures between** at `--frame-rate` 60-240: frame_interp.c's own way (the frame's packets sent again, moved),
  each drawn and scanned out.
- **Measured against `--renderer native --native-draw off`** (title, menus, map 000 at vblanks 300-2700): 29 to
  4,263 of 286,720 pixels differ by more than 8 (blend rounding, the 2D's edges); the torches, the HUD's gauge, the
  menus and the scrolling textures as native. `KF4_PC_CENSUS=1` counts what the game sends (targets, tests, blends,
  textures, transfers) for `--stats`.
- **Save states** (built after): the GS's 4 MB first (the uploads' chunks in order, the colour surfaces' pixels over
  the pages drawn last), then the registers, Q and the CLUT's mirror; taken back as one upload of the whole memory.
  States of the native GS load into the PC renderer and the PC renderer's into the native GS by their first 4 MB
  (gs.c's take_other_vram). Each of the three, loaded at 2600 at map 000, gives at 2700 the picture of the run that
  went on (0 pixels differ). Z buffers are not kept (the game draws its Z anew each frame).
- **Not yet**: native draws (step 3), MSAA, `--native-colour`,
  anisotropic filtering, captures, the overlay's settings; performance not measured yet.

## Step 3, first part (2026-10-29): native draws of program 0x000

`--renderer pc --native-draw on` draws the native draws of program 0x000 itself (`port/gs/pc/pcr_native.cpp`): the
map's records, the sky, the items lying in the maps and the picked item. Each batch's registers (FOGCOL at the set-up,
the A+D packet, PRIM from the strip's tag) are written to the GIF front end as the GS gets them, so the state a draw
leaves is the PS2 path's; its triangles are drawn from the mesh on the GPU (`shaders/pcr_nd000.vert`) through the same
GS pixel as any primitive. VU1's maths is shared with the native GS's shaders (`port/render/shaders/vu1_000_*.h`, moved
out of `nd_000.vert`; the native GS's SPIR-V byte for byte as before), and the draw's record is worked out on the CPU
by `port/render/vu1_record.cpp`, checked bit for bit against VU1's own set-up (`vu1_record_test`, ctest `vu1_record`).

- **The kinds**: gs.c turns on only the kinds the PC renderer draws (`pcr_native_kinds()`); the others go as packets.
  The map's objects wait for program 0x280 (a door at map 000 is a skinned object). `--native-draw-compare` is off
  under it.
- **Only at `--frame-rate 30` for now**: frame_interp.c's CPU way of making the pictures between cannot move a native
  draw (the native GS moves them only on its GPU way), so under the PC renderer the game sends packets whenever there
  are pictures between (`port_gs_native_draws_now`). Step 5 lifts it.
- **Measured** at vblank 2700 against the PC renderer on packets: map 000 110 pixels differ (by 2 at most), 009 374
  (by 3), 009:2 251 (by 2); the sky and the items alone 0. Map 063's clouds differ (14,654 pixels, by 36): the native
  GS's native draws differ from its packets there the same way (14,615), so it is native draw's sky, not this.
- A batch's texture is read whole and what it draws over is taken as its scissor's rectangle (its corners are the
  GPU's), as P7 takes them.
- **Program 0x280** (the skinned models: the map's skinned objects, the creatures, the arm and what it holds, the gold;
  built after): `port/render/vu1_record.cpp` makes its record (both matrix sets, checked bit for bit against VU1's
  set-up in `vu1_record_test`) and VU1's clipping plan of each batch (whether the run draws its strip at all, which
  triangles it kicks); those triangles are listed each time and drawn by `shaders/pcr_nd280.vert` (the maths shared
  with the native GS's `nd_280.vert`: `port/render/shaders/vu1_280_*.h`), the sphere-mapped second pass too. At vblank
  2700 against the PC renderer on packets: 000 111 pixels (by 2), 009 378 (by 3), 009:2 251 (by 2), 063 as before.
- **Programs 0x5A0, 0x700 and the GS strips** (built after). The shadows are 0x000's maths on the silhouette meshes
  with no lights and no fog (`vu1_record_5a0`). The effects (`vu1_record_700`, `vu1_ad_700`: the flipbook's TEX0,
  `vu1_kicks_700`: VU1's clipping with the vertex past the strip) are drawn by `shaders/pcr_nd700.vert`, its maths
  shared with `nd_700.vert` (`port/render/shaders/vu1_700_*.h`). The weapon's trail (`RD_PROGRAM_GS_STRIP`) is
  written to the front end as its GIF packet (`write_packed_tag`, `write_packed_qword`). At vblank 2700 against
  packets: 000 111 pixels (by 2), 009 388 (by 4), 009:2 251 (by 2).

# Part 1: the PC renderer (P1-P6)

A real PC renderer for King's Field IV's world: the map, the map objects, the items, the characters and the arm
drawn by our own Vulkan renderer from the game's data and state, the way a PC game draws its world, instead of
the PS2's GS emulated draw by draw. The GS path keeps what is 2D or over the whole screen (the HUD, the menus, the
text, the fades and the screen passes) and, until they are ported, the effects. `--renderer modern` stays the
reference (the PS2's pictures, checked byte for byte); `fast` is checked against it by how much it differs, not
whether, and is free to differ where that is the point (filtering, depth, anti-aliasing, frame rate).

**Status as written when the plan was closed out** (October 2026; corrected at the top: the native GS's thread, which
"limit no spot measured" then, limits map 000 above about 200 pictures a second at the owner's later settings):
the plan, agreed with the owner (October 2026); P1 built, P2 built (the map's records, objects, items and gold,
opaque, alpha-tested and blended), P3's first two steps built (the skinned parts' first pass drawn once, alphas a
little above 1.0 by Vulkan's blending): at 170 Hz `fast` keeps the owner's settings at 168-170 pictures a second at
maps 000 and 009, where P1 kept 84 and 118 (below, "In real time"). The steps since P2's first were decided overnight
for the owner's review. P4-P6: "Where P4-P6 stand" below; P5's first step and P3's third built (the owner suspended
the rule of agreeing the interface first while the plan is finished). **Where the plan ends** (October 2026): at 170 Hz
every spot keeps 168-170 pictures a second; at 240 maps 003, 004, 009, 013, 017 and 030 keep 239-240, map 000 236 (GPU
bound: 239 at 2x with aniso 1). What is left is not a phase's: the scene's own draws, the Z-first draws of As up to
2.0 (below, "What still has a Z-first draw"), the GS's 2D and screen passes; and on the GS's thread the uploads a
picture between asks for again (P5, "What a recording still costs"), which limit no spot measured.

## Why (measured October 2026, branch `p8/gpu-plan`)

The owner's machine (RX 9070 XT, RADV), the owner's settings (`tools/pace_test.py`'s `OWNER`: modern, 4x with
native widescreen, aniso 16, cut-out AA), the pond (map 009), headless at 240, `KF4_GSN_GPU_TIME=2` (the marks
wait for the work before them: read the kinds as shares):

| | GPU a picture | the scene's colour draws | its alpha-tested Z-first draws |
|---|---|---|---|
| the owner's settings | 5.58 ms | 2.57 | 1.29 |
| aniso 1 | 4.79 ms | 1.95 | 1.10 |
| 2x, aniso 16 | 3.34 ms | 1.24 | 1.01 |
| 1x, aniso 1 | 2.16 ms | 0.52 | 0.74 |

The budget is 4.17 ms a picture at 240 and 5.88 at the owner's 170 Hz. About 550 Vulkan draws and 8,000
triangles a picture. Two costs:

1. **A cost a pixel**: `gs_frag.h` is the GS's texture unit bit for bit (raw texels, each read wrapped and put
   through the CLUT, bilinear four such reads, trilinear eight, `--native-aniso 16` up to sixteen trilinear
   reads). The colour draws scale with the pixels and aniso 16 adds 0.8 ms.
2. **A cost that does not scale**: 2.16 ms at 640 x 448. The ties' Z-first draws, shader blending's copies, the
   turns between the scene and the GS's targets, the render passes: the GS's way of drawing, with the PS2's
   order, state and Z for each batch.

A PC renderer has neither: textures sampled by the hardware, and a world drawn by material with one depth buffer.

## What the game gives us (the decomp)

- **The frame** (`GameLoop_DrawFrame`, `src/game/core/game_loop.c`) draws the world as one block, then the rest:
  `Map_DrawRecords`, `MapObject_DrawAll`, `Entity_DrawPoolModels` (the shadows), `Entity_DrawAll`,
  `MapItem_DrawAll`, `ItemEffect_Draw`, `Gold_Draw`, `Map_DrawLateRecords`, `Effect_DrawAll`, `DrawList_Draw(0)`,
  then with the camera before the eye (`GameLoop_SetCameraMatrices(0, 2)`) `DrawList_Draw(1)` and `(2)` (the arm,
  what is held), and `MapItem_DrawPicked`. `map_draw_state.mode` 1 draws the world mirrored first (the camera's
  mode 1) under a tint; mode 2 draws it into an offscreen target first (the water's reflection). The wobble, the
  tints, the brightness, the fades and the HUD come after the block.
- **The map** is `map_slots[2]` (the map and the one streaming beside it), each with its records already culled by
  the game into `record_draw_lists[map]` and `late_record_draw_lists[map]`. A record is a `ModelInstance`: its
  parts (a tree of `ModelPart`, each a matrix and its data blocks), a light block (two matrices) and a fog block
  from `map_draw_state`, point lights from the linked points that touch it (`LinkedPoint_FindHitsForMapDraw`), its
  tint, fade, alpha and texture (`texture_data`, sent by `TextureTable_QueueEnabled` / `Map_QueueSlotPacket`).
- **The geometry** is the PS2's own format in the files: each part's data is VU1 data blocks (UNPACKs and the
  MSCALs that draw them). `port/render/render.cpp` already parses them into meshes, cached per block.
- **Native draw's adapters** (`game/native_draw/`) already sit at those call sites and turn every draw into an
  `RdDraw` (`port/include/port/render.h`): a stable identity (kind, instance, part, record), its meshes, its
  camera, its matrices, lights, fog and point lights, as VU1's fixed block has them. That is a scene description
  of the frame, made by the game's own code, one draw at a time.
- **The textures** reach the GS as uploads of the game's chunks, and native draw's page-ownership table
  (`gsn_standalone.inc`) already decodes each texture once from the uploads that own it, with its CLUT. The table
  stays the source: it sees every writer (the project tried hooking the loaders and kept the table for that).

## The design

```
game thread (30 a second)                       renderer (each picture)
GameLoop_DrawFrame                              
  world block ── RdDraws (as now) ──► the frame's snapshot ──► PC renderer: views, instances by material,
  (no GS packets for the world)          (views, draws, ids)       hardware textures, one depth buffer
  RD_ND_WORLD mark in the chain ─────────────────────────────► the world composited here, the GS's depth
  HUD, passes, 2D ── GS packets (as now) ─────────────────────► native GS, drawn over the world
```

- **The snapshot.** With `--renderer fast`, the adapters keep making `RdDraw`s but do not queue them in the chain;
  the frame's draws, grouped by the view they are drawn in (the world, the mirror, the offscreen reflection, before
  the eye), go to the renderer whole at the end of the world block. The chain gets one mark there
  (`RD_ND_WORLD`): where the world's picture and depth are put into the GS's frame, so that everything after
  (effects not ported yet, the passes that read the frame, the HUD) draws over it as before.
- **Meshes, persistent.** A mesh (a parsed block) is put on the GPU once and kept while the game keeps its block:
  the map's when the map loads, a model's when it is first drawn. Vertices in the model's space, as the files have
  them; the instance's matrices place them.
- **Materials.** A batch's texture (from the table, expanded to RGBA8 with its mip levels through the CLUT in use,
  kept by texture and CLUT), its alpha test, its blend, whether it writes depth, its fog. Opaque and alpha-tested
  batches drawn by material; blended ones in the game's order within their view.
- **The original's look.** The vertex programs of native draw (`nd_000.vert`, `nd_280.vert`: VU1's lighting,
  the point lights, the fog, the skinning) are the shaders' first stage, and what comes after the texture read
  (TFX, the alpha test, the GS's blend arithmetic where Vulkan's differs) is `gs_frag.h`'s, so a surface is lit,
  fogged and coloured as on the PS2. What differs is only what a PC renderer must or should: the texture
  filtering's precision, the level of detail by the pixel's footprint, depth without the GS's rounding, and
  pictures between. The goal is pictures a player cannot tell from `modern`'s at a glance; the spots' differences
  are reported per phase and each one explained.
- **Room for a remastered look.** The vertex stage passes on what per-pixel lighting would need (the normal and
  the world position beside the lit colour), and the map's light and fog blocks are kept per instance, so a
  remastered look can be a later shader choice without touching the snapshot or the caches.
- **Depth.** One reversed-Z float depth buffer for the world. Coplanar layers (decals, signs, the lava's edges)
  by their order within their record and a depth bias, not the ties' Z-first draws.
- **Pictures between, from snapshots.** The renderer keeps the last two snapshots and draws each picture at its
  time between them: the camera and every instance's matrices interpolated, paired by identity (and by the
  game's own instance where the identity is the model's address: the look-alikes that swap now). No recording and
  no replay for the world; the GS's recording keeps doing the 2D and effects.
- **What stays the GS's**: the screen passes, the HUD, the menus, the text, the movies; the effects (program 0x700)
  until P6. They draw into the GS's frame after the world, with the world's depth given to the GS's Z buffer for
  the effects that test it.

## The phases

Each is playable, measured with the profiler (Shift+P) and `KF4_GSN_GPU_TIME`, and compared with `modern` at the
pond, the fortress, the canyon, map 000 and under water (the picture difference reported per spot). `modern`, the
PS2 path and their checks (RENDERER.md) are not touched.

- **P1: hardware textures.** The texture cache the renderer samples: a table texture expanded on the GPU into
  RGBA8 with its mips, kept by (texture, CLUT), sampled with Vulkan's filtering and anisotropy. Built first and
  usable alone: under `modern` as a switch (`--native-sampler hardware`, the pictures differ in filtering only)
  it takes the per-pixel cost out of today's scene at once, and the PC renderer needs it anyway.
  **Built** (`port/gs/native/gsn_rgba.inc`; `gs_frag.h`'s `GSN_HW_SAMPLER`, built as `gs_hw.frag`, `sa_sample_hw.frag`,
  `gs_in_hw.frag`, `gs_in_ms_hw.frag`; `--native-sampler hardware`, which `--renderer fast` takes). A table texture
  (`sa_table_texture`) keeps its raw texels on the CPU; the first batch that draws it with a given CLUT (or TEXA, for
  CT24 and CT16) expands them as `texel_f` decodes them into an RGBA8 image with its levels, uploaded in the stream as
  the raw image was, and kept by (the decode's serial, the CLUT or TEXA). The scene's program-0x000 batches drawn by
  `sa_draw_batch` read it at binding 6 with a stock sampler: TEX1's MMAG nearest, nearest; linear between levels (MMIN 3
  or 5), the geometry filter with `--native-aniso`'s anisotropy; else bilinear; CLAMP's REPEAT or CLAMP. The level
  comes from the pixel's footprint (no scale's bias; `--native-lod full` still level 0). The GS's sampling is kept for
  a render target or VRAM's texture, a region clamp or repeat, S and T wrapped differently, and the skinned groups
  (`sa_group_state`: their own bindings; not yet measured to be worth it). `--stats`: `native sampler hardware`.

  **Measured** (the pond at the owner's settings, as "Why"): 5.58 -> **3.45 ms** a picture; the scene's colour draws
  2.57 -> 0.73, the alpha-tested Z-first draws 1.29 -> 0.66. 534,274 batches read RGBA8 textures, 134 made (15 MB),
  4,498 kept the GS's sampling (not from the table). The pictures against `modern`'s: a third of the pixels differ, by
  0.43 levels on average and none by more than 16 (the GPU's bilinear has 8 bits of fraction to the GS's 4; the level by
  the footprint); side by side they cannot be told apart.
- **P2: the snapshot and the map.** The `RD_ND_WORLD` mark and the frame's snapshot; the PC renderer draws the map
  records (program 0x000, opaque and alpha-tested) into its own colour and depth, composited at the mark; every
  other world draw still goes to the GS that frame (the turns as now) so the game stays whole while the renderer
  grows.

  **Built, first step** (`port/gs/native/gsn_pc_world.inc`; the snapshot made on the native GS's thread, below, "P2,
  revised"). With `--renderer fast` a scene batch whose corners are all a map record's (`RD_KIND_MAP_RECORD`, program
  0x000, not the far sky) is kept by `sa_draw_batch` once its state is worked out, when it is opaque or alpha-tested (no
  ABE, or ABE that leaves the source: (Cs - Cd) As + Cd with As 0x80 at every pixel, which the map's records set on
  their opaque surfaces), one pass, one run, no shader blending, read by P1's sampler, into the scene's frame (not a
  named target). Kept with its state (`PcKey`: the pushes, masks, scissor, the RGBA8 view and sampler, the records;
  the image and buffers held), and drawn when anything else comes: the same places that draw a skinned model's kept
  parts (`sa_group_draw` now draws both; one of the two is pending at a time), and before any batch the scene draws
  itself. A kept material is one draw: its colour, Z written, GEQUAL, exact (no Z-first draw, no tie slack).
  The corners of a batch are drawn after the draw that made them ends (`sa_flush`): what decides is whether the
  pending corners came from a taken draw (`Sa::pending_pc`, `batch_pc`), not the draw being met. Switches for
  measuring: `KF4_PC_ORDER=1` merges a batch into the first material of the same state, not only the last;
  `KF4_PC_TIES=1` keeps the batch's own draws (its Z first, its colour within the slack). `--stats`: `pc world`.

  **Measured** (October 2026; the owner's settings as "Why", headless at 240, the teleport spots `--teleport
  1900:N:0` after `tests/newgame.pad`, picture 3799, `KF4_GSN_GPU_TIME=2`; GPU ms a picture, two runs each; the
  pictures against P1's, which is `modern` with `--native-sampler hardware`):

  | map | P1 | fast | kept a flush | pixels differing from P1 (worst) |
  |---|---|---|---|---|
  | 000 | 6.43, 6.93 | 6.76, 6.83 | 2.4 | 98 (26) |
  | 003 | 5.23, 5.39 | 5.01, 5.03 | 4.1 | 272 (81: the brazier's glow on the wall behind it) |
  | 009 | 5.96, 6.00 | 5.45, 5.85 | 1.9 | 186 (10) |
  | 013 | 3.71, 3.99 | 3.50, 3.67 | 2.9 | 0 |

  So: about 0.2-0.4 ms a picture where the map is much of the scene, nothing at map 000, and the pictures as P1's but
  for a few hundred pixels where a layer meets a surface. The saving is the kept batches' Z-first draws, not fewer
  draws: a flush keeps 2-4 batches, each its own material (`KF4_PC_ORDER=1` merged 11% more at map 009, measured before the
  ABE rule above). The map's records alternate with what must stay in order between them: their blended layers (58% of
  the taken batches at map 009, ABE with As not 0x80) and the objects of each map slot. What still costs at these
  spots is the rest of the world (the skinned groups, the objects', items' and entities' Z-first draws, the blend
  copies), which P3 takes; and the layers, which need the depth-bias rule below in place of the scene's ties before
  they can be kept. Not yet measured at the owner's quick-save spots (the pond, the fortress, the canyon, under water).
  `modern`'s pictures byte for byte as before (maps 000 and 003; a first run of map 000 before the change differed from
  a second by 64 pixels on its own); ctest 6 of 6, vu1_test 0 of 90 failed; ASan + UBSan (clang) on `fast` at map 003:
  nothing in the changed code. No unit test: the keeping is the scene's state on the GS's thread, checked by the pictures.
  **Built, second step: the blended batches, and the objects, items and gold** (October 2026, decided overnight, for
  the owner's review). Measuring why the taken batches were not kept (map 009) found one case only: a blend that does
  not leave the source, nearly all of it the cut-outs' (alpha test As > 0, (Cs - Cd) As + Cd, Z written: the branches,
  the braziers, the fences; the scene's "Z-first atest", 1.2 ms of the pond-style spot). They are kept now when Vulkan's
  blending does the GS's (no shader blending), drawn whole in one draw: GPU blending and the Z test follow the
  triangles' order, as the GS does, so the scene's Z-first draw and its runs are not needed. The key carries the blend
  (`PcKey`'s `blend_on`, the factors, FIX). And the take is every program-0x000 kind of the world block, not only the
  records: `pc_world_kind`, the map's records, its objects, the items and the gold (`KF4_PC_KINDS`, a mask of
  `RD_KIND_*` bits, for tests); the entities, the arm and the shadows are other programs.

  **The layers' depth.** A blended batch keeps its tie slack (the scene's colour pass's, the two world units of
  `sa_slack` for a layer) and writes Z biased by it. Three other ways were measured and dropped:
  - its Z exact (no bias): map 000's rock over the tower's top (a layer a unit off the bricks, the case `sa_slack`'s
    world units were made for) lost a triangle of itself, 5,700 pixels;
  - the tie's one Z only, not the world units: 4.6% of map 000 differed (the layers far off);
  - the scene's own draws (its Z exact first, then its colour within the slack), whole or run by run: no closer to
    P1, and run by run a draw a triangle (5.2 million at map 000), slower than P1.
  What the bias written costs: a line a pixel wide at the edges of a cut-out in front of what is drawn after it and
  tested against its Z (the braziers' legs against their fire's glow on maps 000 and 003, the edges of map 013's
  roof beams): the line where the slack's Z, not the GS's, decides.

  **Measured** (as the first step's table: the owner's settings, headless at 240, `--teleport 1900:N:0` after
  `tests/newgame.pad`, picture 3799, `KF4_GSN_GPU_TIME=2`; one run each, against P1, `modern --native-sampler
  hardware`):

  | map | P1 | fast, first step | fast | pixels differing from P1 (worst) |
  |---|---|---|---|---|
  | 000 | 6.68 | 6.76, 6.83 | **4.86** | 1,916 (219: the brazier's legs, above) |
  | 003 | 4.95 | 5.01, 5.03 | **3.79** | 8,419 (215: the braziers' legs and rims) |
  | 009 | 5.64 | 5.45, 5.85 | **4.88** | 2,129 (17: specks in the fire) |
  | 013 | 3.84 | 3.50, 3.67 | **3.19** | 1,468 (45: the roof beams' edges) |

  So 0.65-1.8 ms a picture under P1: the alpha-tested Z-first draws 1.23 -> 0.77 ms at map 009, the scene's colour
  draws 1.09 -> 0.90. A flush still keeps few materials (batches kept 62,041 and materials 59,070 at map 009: merging is
  rare while the chain's order is kept; `KF4_PC_ORDER=1` is the switch for trying more), so what saves is the Z-first
  draws and runs left out, not fewer draws. `modern`'s pictures byte for byte as before (maps 000 and 003, against the
  commit before); ctest 6 of 6, vu1_test 0 of 90 failed; ASan + UBSan (clang) on `fast` at map 003: the sound driver's
  and the decomp's known reports only, nothing in the changed code. No unit test (the keeping is the scene's state on
  the GS's thread, checked by the pictures). No clang-tidy: the project has no configuration for it.
- **P3: the whole world.** Blended and layered records, map objects, items, gold, entities (0x280, skinned), the
  sky, the shadows, the arm and what is held (the eye's view), the picked item. The GS then draws no world. (The
  blended and layered records, the objects, the items and the gold: P2's second step, above.)
  **Built, first step: a skinned part's first pass drawn once** (October 2026, decided overnight, for the owner's
  review). What the scene still draws itself at map 009, counted by triangle: the skinned models' (program 0x280: the
  entities, the arm) 8.6 million with a Z-first draw, far more than the rest (the shadows 3.8 million, by the stencil;
  the reflection's water 0.6 million; the 0x000 batches blended in the shader 0.2 million). The skinned groups (pf/skin)
  draw a group's first passes twice, their colour with no Z written and then the same draw exactly with Z. A part's
  slack is 0 (`skin_eps`), and every pixel passes the alpha test (role 1's `covers`), so the second draw writes, last,
  every pixel the first did: the second alone leaves the same colour and Z. With `--renderer fast`
  `sa_group_first_draw` leaves the first out when the two draws are the same but for the Z write
  (`KF4_PC_GROUP_ONE=0`: both). The pictures byte for byte as with both, at maps 000, 003, 009, 013 and 017; GPU ms
  a picture (the owner's settings, as P2's table):

  | map | both | one |
  |---|---|---|
  | 000 | 4.84 | **4.53** |
  | 003 | 3.80 | **3.59** |
  | 009 | 4.89 | **4.66** (the skinned groups 0.63 -> 0.40) |
  | 013 | 3.17 | **2.92** |
  | 017 | 2.74 | **2.63** |

  It would give `modern` the same pictures for the same saving; it is left to `fast`, as `modern` is not touched
  (the owner's call whether it should have it). `modern` byte for byte as before (map 009); vu1_test 0 of 90 failed;
  ASan + UBSan (clang) on `fast` at map 009: the known reports, and one in native draw's water (a signed overflow
  in the texture coordinates, `gsn_nd_water.inc`), fixed in its own commit.
  **Built, second step: alphas a little above 1.0 blended by Vulkan** (October 2026, decided overnight, for the
  owner's review). What the scene still drew with a Z-first draw at map 009 after the first step was nearly all in the
  pictures between (the skinned models are drawn only there at 240: the frame's own picture keeps them for the
  pictures between): the creatures' parts whose textures have alphas up to 0x88 (their CLUTs' 0x84 and 0x88), so As
  above 0x80 and (Cs - Cd) As + Cd blended in the shader (`as_over`), each run of a part a copy of the scene and two
  barriers: "scene blend copies between" 0.65 ms, and their runs' Z-first draws. `modern` pays the same (0.49 ms).
  With `--renderer fast` `pc_world_alpha_over` lets Vulkan's blending take As up to 0x90: it clamps the factor to 1.0,
  so such a pixel comes out Cs, not Cs + (Cs - Cd)(As - 1.0), at most an eighth of (Cs - Cd) off (`KF4_PC_ALPHA_OVER`,
  128 for the GS's rule). At the creatures (009:2, `--teleport 1900:9:2 --invincible`, two of them close): 3% of the
  pixels differ, by 3 levels at most (their shirts and hair), and

  | spot | P1 | fast, before | fast |
  |---|---|---|---|
  | 009:2, the creatures | 5.46 | 4.54 | **3.15** |
  | 000 | 6.68 | 4.53 | **4.52** |
  | 003 | 4.95 | 3.59 | **3.58** |
  | 009 | 5.64 | 4.66 | **3.64** (copies between 0.65 -> 0.04, Z-first atest 0.77 -> 0.20) |
  | 013 | 3.84 | 2.92 | **3.00** |
  | 017 | 3.41 | 2.63 | **2.53** |

  (GPU ms a picture, the owner's settings, as P2's table; map 009's place 0 draws the creatures in the pictures
  between though none is in view.) The pictures against P1's at places 0 as P2's second step's, 017 660 pixels
  (worst 37). `modern` byte for byte as before (009:2, its frame and a picture between); ASan + UBSan (clang) on
  `fast` at 009:2: the known reports only.
  **In real time** (`tools/pace_test.py --maps 0,9 --hz 170 --mode pace`, the owner's settings at 170 Hz, one run
  each, October 2026, after P3's second step):

  | map | P1 (`-- --native-sampler hardware`) | fast (`-- --renderer fast`) |
  |---|---|---|
  | 000 | 84 pictures a second (lowest second 58), the game slowed to 41 vblanks a second | **168** (lowest 161), 60 |
  | 009 | 118 (lowest 98), 42 vblanks a second | **170** (lowest 168), 60 |

  The back end's thread (the Vulkan calls, `gsn_backend.inc`) does much less: 10.5 -> 3.4 s of a 3,800-vblank run at
  map 000, 4.7 -> 2.6 s at map 009; the native GS's thread's time a picture between (drawn from the packets) is as
  before (5.8 ms at map 009, `--fast`), which is P5's.
  **Built, third step: the skinned groups read P1's textures** (October 2026). P1 left the skinned groups
  (`sa_group_state`, pf/skin) on the GS's sampling: their key bound the raw texture and the CLUT. At map 000 at 240 the
  GPU was the limit (2x and aniso 1 kept 239 pictures a second where the owner's settings kept 233), and the groups were
  the second largest kind. With `--renderer fast` a group's key carries the RGBA8 view and sampler `sa_draw_batch` made
  for the batch (`SaGroupKey::rgba`, the image held by the pass), drawn with `programs_hw` (the shines blended in the
  shader with `programs_in_hw`); `KF4_PC_GROUP_HW=0`: the GS's sampling. The groups' GPU time (headless at 240,
  `KF4_GSN_GPU_TIME=2`): map 000 0.78 -> 0.53 ms, 009:2 0.56 -> 0.29; a picture 4.20 -> 3.96 and 3.10 -> 2.82. The
  pictures differ in filtering only, as P1's: 0.7% of map 000's pixels by 3 levels at most, 4.5% of 009:2's (the creatures)
  by 9 at most. `modern` byte for byte as before (009:2, frame and pictures between); ASan + UBSan (clang) on `fast` at
  009:2: the known reports only. At 240 in real time map 000 233 -> 236.5 pictures a second, 009 239.

  **Measured and left: the effects, and the skinned plans** (October 2026, overnight). Two things tried after the
  second step, neither kept on:
  - the effects (program 0x700) taken by the PC world (`pc_world_takes`; a key now carries its corners' vertex program,
    `pc_corners_vs`): the pictures the same, the GPU time the same within the runs' noise (map 009 3.76-3.78 against
    3.59-3.85 without; 000 4.50, 013 2.94): an effect has no Z-first draw to leave out and is a material of its own. Off by
    default; `KF4_PC_KINDS=0x9E` takes them, for P6.
  - the skinned plans' cache four plans a batch instead of two (the world's and the reflection's, two models of a mesh):
    at 009:2 the plans found 7.37 million vertices against 7.36 million, so the plans planned again are new ones, not
    two models taking each other's. Dropped.

  **And one guard:** what the PC world keeps is drawn before a batch for another frame target binds that target
  (`pc_world_flush_for`, at `sa_draw_batch`'s start, as the skinned groups have it): the kept materials draw into the
  target bound when they are flushed. It never fired at the spots (`--stats`: `pc world: ... flushes (N for another
  target's batch)`), so no picture changed, but nothing kept it from coming. And the sweep: `--renderer fast` at every
  map 000-063 (`--teleport 1900:N:0 --invincible`, 400 vblanks after, the owner's settings): no crash, no error.
- **P4: reflections.** The mirror (mode 1) and the offscreen reflection (mode 2) as views of the same scene; the
  rippled water (`RD_PROGRAM_RIPPLE`), the scrolling textures.
- **P5: pictures between from snapshots.** The world's pictures between by interpolation; the recording keeps
  only the GS's part.
- **P6: effects.** Program 0x700's draws as instanced particles; then the GS keeps only the 2D and the passes.

## Where P4-P6 stand (October 2026, overnight, for the owner)

P2 and P3's steps grew the PC renderer inside the native GS's scene (the materials kept and drawn as the chain meets
them, P2 revised), not as the separate snapshot renderer the phases above were written for. With that, the speed the
phases were for is mostly here already: at 170 Hz every spot measured keeps 168-170 pictures a second (maps 000, 003,
004, 009, 013, 017, 030; map 021 draws no pictures between with P1 either). What is left of each phase, measured:

- **P4, reflections.** At every place measured (009:0-6 and the spots) the reflection costs nothing on the GPU ("refl"
  kinds 0.000-0.003 ms): the scene draws it into its named target (T3) and the pictures between keep the frame's
  (`sat_kept`: about 500,000 batches a run left out). What remains is the water's surface (`RD_KIND_WATER`, the ripple
  program, its own bindings: `ndw_batch`), part of 0.2 ms of Z-first draws at map 009. Not taken by the PC world: little
  to win at the spots.
- **P5, pictures between from snapshots.** The one large cost left is the native GS's thread: about 5.8 ms of CPU a
  picture between at map 009 (`--fast`, drawn from the packets), the same with P1 and `fast`. Its profile (perf, map
  009, the GS's thread 54% of the program's samples) is flat: memmove 6% (most of it the uploads' data copied, 1.8%),
  the skinned plans 5.4%, VRAM reads 3.9%, `sa_draw_batch` 2.4%, the native plans 2.3%, then many under 2%: the GIF
  front end, the uploads (9.3 GB a run, 168,000 asked for again by the pictures between), the batches' work. No one
  change takes much of it. **First step built**: the draws the PC world takes are drawn again from the frame's kept
  batches (below, "P5, built"), 11-23% of a picture between.
- **P6, effects.** Taken by the PC world, the effects (program 0x700) changed no picture and saved nothing on the GPU
  (P3's "Measured and left", above; `KF4_PC_KINDS=0x9E`). Instanced particles would save the GS thread's work on them
  (part of P5's cost), not the GPU's: with P5.

**What still has a Z-first draw** (map 000, the GPU's 0.76 ms of "scene Z-first atest", counted where the scene draws a
batch itself, after the PC world and the skinned groups took theirs): 2.4 million triangles of a run are 0x000 batches
blended in the shader because their As goes up to 2.0 (textures' alphas to 0xFF, (Cs - Cd) As + Cd), and 0.3 million
the PS2 path's in pictures between. Vulkan's blending cannot do As above 1.0 exactly (a UNORM target clamps the factors
and the output to 1.0, and one blend equation cannot add Cd (1 - As) below 1.0 and take Cd (As - 1) away above it), so
these stay the shader's, as in P1. (Taken first for the semi-transparent creatures: the blended 0x280 batches counted
there were the skinned groups' shines, whose Z draws the groups leave out already; tried and dropped.)

### P5, built: a taken draw's kept batches drawn again in its pictures between (first step)

(October 2026; the owner suspended the rule of agreeing the interface first while the plan is finished.) The frame's
first picture between walks its packets again (rc/own, `nd_prep_*`), and a draw the PC world takes was worked out
again there: its corners copied, its scene batch's state, texture, alphas and blending, to come to the frame's key. Now
(`gsn_pc_world.inc`, after the P2 code):

- **The frame keeps**, for each draw the PC world took (by its place among the frame's native draws, as `nd_prep` keeps
  its records) what `pc_world_keep` was given for each of its batches (`PcKept`): the key, the texture, the context of
  its FRAME and ZBUF, the rows, and every triangle of the batch in order, its slope in its alpha as `sa_draw_batch` worked
  it out (`sa_slope`, now a function), those left out at the scissor marked. A batch it drew nothing of (every
  triangle outside the scissor, ZTST NEVER, an empty scissor) is kept as that, without a key. A draw is whole when every
  batch `sa_draw_batch` met ended in one of these.
- **A picture's draw** at that place, with the same value and its record taken (`nd_prep_take_const`), whole and in
  the same arena (`pc_between_takes`): `nd_draw` makes its record (the picture's moves) and writes the batches'
  registers as before, but plans no corners; `pc_between_draw` gives each kept batch to `pc_world_keep` with the
  picture's record in its corners, into the picture's own FRAME and ZBUF (the buffer shown, not the frame's), with
  `sa_flush`'s and `sa_draw_batch`'s work around a kept batch (the reflection kept, the group's parts, the target's
  flush, `sa_acquire`, the rows). The triangles the frame left out at the scissor are drawn too: the GPU clips them
  (a picture's `sa_draw_batch` keeps one in view at t 0; testing them cost a quarter of the picture's time, drawing
  them nothing measurable on the GPU). A batch with no key and a triangle in view at t 0 (`nd_box_at_0`, its work now
  `nd_box_at_0_of` for any corners) makes the draw worked out as before.
- **A draw's batches end with it** while frames are kept for their pictures between (`pc_between_cuts`, frame and
  picture alike), so that each kept batch is one draw's. That changes the frame's batches (a batch is one draw's
  alphas, so opaque where a longer one was blended): at map 000 70,800 pixels differ from before, 95 by more than 16
  levels (a brazier's rim, up to 219), at map 003 416 (the braziers' rims), none at 009:2, 013 or 017; at 30 nothing is
  kept and nothing changes. `KF4_PC_BETWEEN=0`: neither, `fast` as before byte for byte.
- `--stats`: `pc world, pictures between`: the draws drawn from the frame's kept batches, and those worked out again
  (not every batch kept: a batch the PC world did not keep; a triangle of a batch with no key in view at t 0).

**Measured** (the owner's settings, `--fast` at 240, `--teleport 1900:N:P` after `tests/newgame.pad`, picture 3799; a
picture between drawn from the packets, from `--stats`):

| spot | before | P5's first step | GPU a picture |
|---|---|---|---|
| 000 | 5.67 ms | **4.46** | 3.64 -> 3.42 |
| 003 | 2.85 | **2.19** | 2.08 -> 2.07 |
| 009 | 5.61 | **5.00** | 2.96 -> 2.95 |
| 009:2, the creatures | 3.07 | **2.68** | 2.25 -> 2.23 |
| 013 | 2.81 | **2.36** | 2.11 -> 2.08 |
| 017 | 1.87 | **1.63** | 1.50 -> 1.50 |

Real time at 240 (`tools/pace_test.py --maps 0,9 --hz 240 --mode pace,cost -- --renderer fast`, one run each):

| map | pictures a second (lowest second) | a recording | the GS's thread a game frame |
|---|---|---|---|
| 000 | 197 (187) -> **231 (224)** | 6.35 -> 4.81 ms | 22.2 -> 20.1 ms |
| 009 | 240 (237) -> 240 (237) | 5.87 -> 5.22 ms | 22.1 -> 21.7 ms |

At maps 009, 013, 017 and 009:2 every picture (the frame's and `_250`, `_500`, `_750`) is byte for byte the step's
without the replay (`KF4_PC_BETWEEN=0` at the same commit: map 009, 000), and byte for byte before at 013, 017, 009:2.
`modern` byte for byte as before at map 009 (frames and pictures between); at map 000 one picture between differed by 64
pixels, as two runs of map 000 already did. ctest 6 of 6, vu1_test 0 of 90 failed, boot_test PASS, frame_test
`--renderer native` and `fast` PASS. ASan + UBSan (clang) on `fast` at map 003 with the pictures between (50,052 draws
drawn from the frame's): the sound driver's, the decomp's and the port's known reports only, nothing in the changed
code. No unit test: what is kept is the scene's state on the GS's thread, checked by the pictures. No clang-tidy: the
project has no configuration for it.

**What a recording still costs** (perf, map 009, after the step): the uploads asked for again about 27% (each written
to VRAM again and its blocks given back to the table: the GS's part, which every draw that is not the PC world's
reads), the test at t 0 of the batches the frame drew nothing of about 15% (as before: the old path did the same), the
skinned models' draws (0x280) and their groups, the PC world's flushes. The draws' registers are still written as the
frame wrote them (the GS's state after the world is the frame's).

The checks on the branch as it stands: `tools/boot_test.py` PASS; `tools/frame_test.py --renderer native` PASS, and
`--renderer fast` (added to the tool with `modern`) PASS: `fast`'s pictures the same every run, and the last one
dumped alone the same.

## The first interface: P2, revised (for agreement)

**What the survey found** (October 2026): the snapshot cannot be made on the game's side as first planned. A draw's
material is not in its `RdDraw`: its texture is the GS's TEX0 (in the batch's A+D packet) read against VRAM *as it
is at that point of the chain*, and its TEST and ALPHA are the chain's registers. `Map_DrawRecords` uploads map 0's
textures, draws map 0's records and objects, then uploads map 1's (packets 0, 1, 5 of each slot) and draws map 1's:
a world drawn whole at the block's end would read map 1's pages for map 0's records. The page-ownership table is
the only place that knows which texture a draw sees, and it lives on the native GS's thread, in chain order.

So the snapshot is made **on the native GS's thread, as the draws are met**: the adapters keep queuing `RD_ND_DRAW`
as now; the native GS, meeting a draw the PC renderer takes, resolves its batches' materials then (the texture from
the table, expanded by P1's cache, kept by a strong handle so a later eviction cannot drop it; TEST, ALPHA, FBA, FOG,
the scene target) and keeps the draw instead of drawing it. What it keeps is drawn, by material, into the scene's
colour and D32F depth (the standalone scene's own targets, `gsn_scene_targets.inc`) when anything else is about to
touch the scene: the first draw it does not take, a PS2-path batch, a scene op, a turn back to the GS, a vblank. This
is how `sa_group_draw` already keeps a skinned model's parts. In P2 that keeps the chain's order exactly (map 0's
objects draw after map 0's records), with no change on the game's side; from P3, when the PC renderer takes the
whole world, the one flush a frame is at the world block's end, and the `RD_ND_WORLD` mark is added then (the game's
`FrameInterp_EndWorld` hook is where it goes). The game-side `rd_world_begin` / `rd_world_end` are dropped.

On the native GS's side (`port/gs/native/gsn_pc_world.inc`, included by `gsn.cpp` after `gsn_standalone.inc`, in its
namespace, like the scene's; the native GS's thread only):

```cpp
// ---- The PC renderer's world (--renderer fast, docs/port/PC_RENDERER.md) -----------------------------------------
// The world's draws the PC renderer takes are kept as they are met in the chain, their materials resolved then from
// the GS's state and the page-ownership table, and drawn by material into the scene's colour and depth at the next
// flush. P2 takes the map's records (RD_KIND_MAP_RECORD, program 0x000) whose every batch is opaque or alpha-tested
// (no ABE, no FBA trick, no shader blending, TME from the table, into the scene's frame); every other draw is
// drawn by the scene as now, after the flush.

// A batch's material: what the draws are sorted and bound by
struct PcMaterial {
    ImageHandle texture;   // P1's RGBA8 with its levels (rgba_view's image, held: eviction cannot drop it), or none
    const Sampler *sampler;
    uint32_t test;         // TEST's alpha test and Z test (ATE, ATST, AREF, AFAIL, ZTST)
    uint32_t tfx_fog;      // TEX0's TFX and TCC, PRIM's FGE
    bool zwrite;           // ZBUF's mask
};

// Meeting RD_ND_DRAW (sa_draw_begin's place): true if the PC renderer took it (kept, nothing drawn now); false and
// the scene or the GS draws it, after pc_world_flush
bool pc_world_take(uint64_t value);
// What was taken drawn into the scene's colour and depth, opaque by material, then alpha-tested by material, the
// draws' order kept within a record (coplanar layers by order and a depth bias, not the scene's Z-first ties)
void pc_world_flush();
// a map change, a save state loaded, the renderer switched: what is kept dropped, the meshes on the GPU forgotten
void pc_world_forget();
void pc_world_print_stats(); // --stats: draws taken, batches, materials, flushes a frame, the GPU's meshes
```

- **Meshes on the GPU** stay in native draw's arena (`nd_place`, cached by `RdMesh`): already persistent. A batch is
  one `vkCmdDraw` with its first instance naming its draw's `NdConst` record, so a material's batches share their
  bindings and pipeline; `nd_000.vert` (built as `sa_000.vert`) is the vertex stage, `invariant gl_Position` so
  that a layer and its surface give the same depths.
- **Checks for P2**: `modern`'s pictures byte for byte as before; `fast` at the five spots against `modern`
  (mean and worst difference, per spot, each explained); the pond's GPU time at the owner's settings against P1's
  3.45 ms; ctest, vu1_test, ASan + UBSan.
- **Later phases**: P5's pictures between interpolate the kept draws (their `RdDraw` identity and matrices are in
  the snapshot as before); only where the snapshot is made has changed.

## P7: the world off the GS's batches (built 2026-10-29)

**What stopped the last attempt, and how it is dealt with.** A PC renderer apart from the GS was stopped by "P2,
revised": a draw's material (TEX0 read against VRAM as the chain has it at that point, TEST and ALPHA the chain's
registers then) is only known on the GS's thread, in the chain's order. P7 keeps that: the material is still resolved
where the GS's thread meets the draw. What it takes away is everything else the GS did with the batch: no corners
planned on the CPU, no GS batch, no `sa_draw_batch`, no runs, no ties' Z-first draws, no texture rows from the
vertices. The batch is drawn at once, in the chain's order, from its mesh on the GPU.

`port/gs/native/gsn_pc_draw.inc` (included by `gsn_standalone.inc` after `gsn_pc_world.inc`; the GS's thread):

- **Program 0x000** (the map's records and sky, objects, items, gold, the picked item): one draw a batch, its triangles
  a list on the GPU made once per mesh (`pcd.tris`: the arena vertex of each corner and the kick bits), the draw's
  `NdConst` record its instance; `pc_000.vert` (`nd_000.vert` built with `PC_DRAW`) does VU1's maths, the ADC that
  leaves a triangle out and a layer's Z slope. Consecutive batches with the same material and record grow the last
  draw (`Cmd::grow_last_draw`): at map 000 2.24 million batches in 2,500 vblanks became 602,000 draw calls.
- **Program 0x280** (the creatures, the arm and what it holds): the CPU's plan (`nd280_plan_cached`) still says whether
  a run draws and which triangles VU1 kicks; those go into a list each time (`pcd.kicked`), each pass one draw with
  `pc_280.vert`. No corners, no GS batch, no skinned groups.
- **Program 0x700** (the effects): the same, one strip a draw with `pc_700.vert`, the layer's slope from the plan.
- **Materials** are cached by the registers they are resolved from, the vertices' alpha range and the CLUT's contents
  (`PcdKey`); dropped when the page-ownership table changes. A material that reads a render target or VRAM (the
  reflection, the scrolling textures, a region clamp) uses the GS's sampling (`gs_frag.h`) and is resolved again
  each time. A draw into a named target (the reflection's 256 x 256) is drawn there.
- **Declined** to the GS's batch as before: shader blending (As above 1, PABE, no COLCLAMP, FBA; about 3% of the
  batches at map 000, 8-12% at 009 and 063), two alpha-test passes, DATE. `KF4_PC_DRAW=0` turns P7 off.
- The pipelines (both samplers, `ps_scene_samples()` x the scratch formats) are in the pipeline set: `KF4_GSN_WARM=set`
  made none outside it.

**Measured** (the owner's settings: 4x, native widescreen, MSAA 2, aniso 16, cut-out SSAA, `--native-colour high`;
headless, `pace_test.py`, P7 against `KF4_PC_DRAW=0`, runs alternated):

| | map 000 | map 009 |
|---|---|---|
| the GS's thread a game frame (cost, 240) | 13.1 ms, was 16.2 (-19%) | 13.9 ms, was 17.0 (-18%) |
| a recording | 4.83 ms, was 4.34 | 4.2-4.3 ms, was 4.63 |
| fixed 240: pictures a second (lowest second) | 239.75 (239), was 236-238 (230-234) | 239.75, as before |
| the GPU a picture at 240 | 3.40 ms, was 3.61 | 3.40, as before |
| unlocked: pictures a second | 256-258, was 198-203 (+28%) | 262-264, was 225-228 (+16%) |

**Pictures** against the GS's batches (2,500 vblanks, the frame and three pictures between): map 000 differs by one
level over whole surfaces (the GPU's interpolation of colour and UV against the GS's corners: 270,345 pixels, worst
18, 2 above 16); 009:0 22 pixels (worst 4); 009:2's creatures 2,374 by one; 063 6,420, of which 127 above 16: a seam
crack the GS's batches drew and P7 does not.

**What still goes through the GS's batches in `fast`**: the declined batches above; the shadows (0x5A0: DATE and the
stencil, about 570 batches at map 000); the GS's own 3D (about 7,000 batches); the water's ripple and the weapon's trail
strips; the 2D and the screen passes (by design, part 1's top). A picture between still walks the frame's packets
again (the recording), and at map 000 P7's recording is about 0.5 ms dearer, its many small draws recorded as calls
rather than replayed GS batches: "Where it stands", next item 1.

### P7, kept: the frame's calls made again in its pictures between (built 2026-10-29)

A picture between walked the frame's packets again and, for each world draw, resolved every batch's material again and
planned its triangles again, for the picture's record. Now the frame keeps what P7 did, and a picture makes it again
(`gsn_pc_draw.inc`, "Kept calls"; `PcdKept` in `gsn_nd_prep.inc`, beside the record and corners nd_prep keeps):

- **The frame keeps each call.** Every P7 call of a draw goes into `NdPrepFrame::pcd_calls`, in the draw's order: a batch
  of program 0x000, a pass of 0x280, a strip of 0x700, and those that drew nothing (`PCD_CALL_NOTHING`) or were not
  kicked or all clipped (`PCD_CALL_UNKICKED`, no PRIM). A drawn call holds its material (`shared_ptr`: the material cache
  may let it go), its triangles' list (a buffer never written again), its first triangle and count, its instance bits
  and the registers its material was resolved from. A draw is kept whole when every one of its batches went to P7 and
  was kept; a declined batch, a material that reads a target or VRAM (`!kept`), an unsupported mesh, or an arena that
  changed while the draw was made, makes it not whole.
- **A picture takes the draw** when the frame's draw at that place is whole, the table has changed no more since the
  frame began than the frame's had (`sat.gen`, counted from each frame's own start: the counter goes on from frame to
  frame) and the arena is the frame's (`pcd_between_takes`).
- **Each call is made again** (`pcd_between_call`). Its registers are checked against the picture's, less what the
  picture has of its own: FRAME's base and width and ZBUF's base (a picture is shown in its own buffer), the scissor
  and the scene's places. Then the place part of `pcd_prepare` runs as it does for any batch (`pcd_where`: what came
  before in the chain is drawn, a reflection kept as the frame drew it, ZTST NEVER, `sa_acquire` from the picture's
  FRAME and ZBUF, the viewport and scissor), and `pcd_finish` draws with the kept material and triangles and the
  picture's record. Nothing is resolved or planned: `nd280_plan_cached`, `nd700_plan` and the corners are not made for a
  taken draw, and 0x700 learns from its kept call whether a strip is kicked (PRIM is written only then).
- **`pcd_prepare` is three parts**: `pcd_key_regs` (the registers, and the declines that draw nothing), `pcd_where` (what
  came before, the target, the place) and `pcd_material` (the material, held when kept). A kept call uses the first for
  its check and the second for where it goes; the third is the frame's, kept.
- **A fallback** (a call whose registers or place differ, or that declines in the picture) turns the draw back to the
  normal path from that call on (`pcd_between_stop`). The calls before it were checked and stand.
- `--stats`: `pc draws, pictures between (KF4_PC_KEPT)`: the draws made again from the kept calls, and those worked out
  again, by why (no frame kept, not whole, the table or arena changed, a call on: its registers differed or it declined).
- `KF4_PC_KEPT=0`: nothing kept and nothing taken, `fast` as before.

**Checked**: every picture (the frame's and `_250`, `_500`, `_750`, every 7th vblank from 2300 to 2650, 204 a spot) byte
for byte the build's before at 000, 009, 009:2 and 063, and `KF4_PC_KEPT=0`'s; frame_test `--renderer native` and
`fast` PASS; ASan + UBSan (clang) at 000 and 009:2, nothing in port/gs.

**Measured** (the owner's settings, `pace_test.py --hz 240 --mode cost`, alternating runs, two each):

| | kept on | `KF4_PC_KEPT=0` |
|---|---|---|
| 000: a recording | 4.85, 4.79 ms | 5.05, 4.93 ms |
| 000: the GS's thread a game frame | 12.79, 12.81 ms | 12.92, 12.97 ms |
| 009: a recording | 5.13, 5.22 ms | 5.12, 5.10 ms |

About 3% of a recording at 000, nothing at 009, because the table check turns most draws away: made again / turned away
by it, 60,046 / 112,491 at 000, 42,794 / 187,606 at 009, 45,669 / 58,410 at 009:2, 86,668 / 0 at 063.

**Open: the table check is a count, and a count cannot say "unchanged".** A picture between changes the table less than
its frame did: it asks only for the uploads it needs again (frame_interp.c's uploads left out), leaves the kept
reflection's draws and the frame's copies out, and draws into the buffer shown. Counting only the named targets' rows,
or only the owners' changes (the uploads and the table made again), turned away the same draws (tried, not kept). With
the check removed (a test run at 009:2) twice the draws were taken and the pictures were the same, but that is not a
proof. The owner's rule (2026-10-29): no shortcut and no workaround; the check is replaced by one of what the call reads
(the table's answer for its texture's pages, the same image as the frame's), not removed or loosened. That check is
part of the next step ("Where it stands", "Next"), where the kept calls become the world's snapshot.

### The world's snapshot: a picture's world draws without the walk (built 2026-10-29)

"Where it stands", next item's steps 1-3, built on the kept calls ("P7, kept", above). `gsn_pc_draw.inc`, "Snapshot";
`PcdSnapDraw` in `gsn_nd_prep.inc`.

**What the frame keeps.** For each draw kept whole (every batch the PC renderer's, every call kept), besides its calls:
the draw and camera as fetched; the 33 registers it began with (`pcd_entry_regs`: everything a call or the record reads
but the vertex, transfer and port registers, FRAME and ZBUF less their bases and FRAME's width, and the scissor: a
picture's own); each TEX0 write among its calls (`pcd_batch_write`: the CLUT load it starts, with TEXCLUT then, and
before which call); and the registers its writes left (those written, and those a write changed besides: TEX0's
mipmap addresses). A draw that writes a vertex, transfer or port register, or FRAME, ZBUF or a scissor, or one of whose
calls drew nothing for its place (the kept reflection, an empty scissor), keeps none.

**What a picture does with it** (`pcd_snapshot_find` at `RD_ND_DRAW`, `pcd_snapshot_draw` in place of the batches):
1. **Its place in the chain.** When the 33 registers are the frame's at the draw's start (exact), the draw is the frame's
   from there: its writes are the meshes' and the draw's, the same data, so every call's registers are the frame's
   (the per-call check of the registers is not needed). Its record is made from the kept draw and camera (`nd_fetch`:
   no `rd_fetch`, no meshes); the GS's vertices pending are drawn first and the scene's kept parts after the loads
   before the first call, as the walk's first call does; then the calls, with their materials, by the picture's
   record. One `pcd_where` for the draw: every drawn call's frame, Z buffer and scissor (its context's, the picture's
   bases) must be the first's.
2. **The state it leaves.** The CLUT loads are made again in order (they read VRAM: `clut_load`, `sa_clut_load`), each
   drawn call's CLUT checked as the frame's after the loads before it (the mirror's hash); then the registers are set
   as the writes left them, without writing them.
3. **The records.** Each draw's record is made from the kept draw and camera, as the walk made it from the fetched ones
   (the same code: the moves by `nd_model_flags`), with no fetch and no batch walked.

**Exact checks** (none is a count): the registers at the draw's start; each drawn call's CLUT; the place (`pcd_where`'s,
and each call's `pcd_place_same`); the table's answer for each call's texture (`pcd_table_same`: `sa_texture` asked as
the material was resolved, the same decode by its serial, not a target; found once a material while the table, the frame
copy and the scene's image are as then). A CLUT load from a page some target has walks the draw (its write-back would be
an act among the draws). Anything that fails walks the draw as before, its loads undone first (the GS's and the table's
CLUT put back). **The kept calls' table count is replaced** by `pcd_table_same` in `pcd_between_call` too: at 000 every
kept draw is now taken (172,537; was 60,046 with 112,491 turned away by the count).

`--stats`: `pc draws, snapshot (KF4_PC_SNAPSHOT)`: the draws made from the snapshot, their calls and loads, the draws
walked by why, and the frame's draws kept whole without a snapshot by why. `KF4_PC_SNAPSHOT=0`: none kept, all walked.

**Checked**: at 000, 009:0, 009:2 and 063 (the start: hex 0x63, 99 to `--teleport`, where the game starts; 204 pictures
each: the frames and `_250`, `_500`, `_750`, every 7th vblank 2300-2650) byte for byte with `KF4_PC_SNAPSHOT=0` and with
the build before; ctest (gsn_draw among them), vu1_test, boot_test, frame_test native and fast; the map tour (64 maps,
its captures made the first time on this machine in 9 min 17 s: 450,157 pixels, worst 106 on map 030, every scanout
the same; the same with the build before); ASan + UBSan (clang) at 000 and 009:2, nothing in port/gs. Made from the
snapshot: 169,380 draws at 000 (208 walked for their registers, 2,949 for a CLUT on a target's page), 226,187 at 009
(474, 3,739), 100,565 at 009:2 (569, 1,608, 1,337 for the place or the table).

**Measured** (the owner's settings, `pace_test.py --hz 240 --mode cost`, alternating, two runs each):

| | snapshot | `KF4_PC_SNAPSHOT=0` |
|---|---|---|
| 000: a recording | 4.37, 4.47 ms | 4.62, 4.65 ms |
| 000: the GS's thread a game frame | 12.42, 12.32 ms | 12.65, 12.62 ms |
| 009: a recording | 4.49, 4.55 ms | 4.94, 4.81 ms |
| 009: the GS's thread a game frame | 14.58, 14.53 ms | 15.34, 15.03 ms |

## Open question

- **P1 under `modern` too** (`--native-sampler hardware`): the plan builds it so, so every map is faster before
  P2-P6; its pictures differ from the PS2's in filtering only. Not decided: `fast` uses it either way.

# Part 2: smooth play (steps 0-5)

The owner's report: on a top GPU (RX 9070 XT, 170 Hz screen) the game still drops pictures, stalls, and logs
`a pipeline not on the list was made in play`; it should be much better than it is, and have an unlocked frame
rate. This plan takes each cause measured below to its fix, in order of what the owner feels, and adds
`--frame-rate unlocked`.

## Smooth play: what was measured (main 74149928 + the refresh clock, 2026-10-28)

Headless, `KF4_PRESENT_SIM=169.83` (the owner's screen), the owner's settings from `settings.json`: `--renderer
fast --native-draw on --native-scale 4 --native-widescreen --native-msaa 2 --native-aniso 16 --native-cutout-aa
ssaa --native-colour high`, newgame.pad and `--teleport 1900:MAP:0 --invincible`, 30 s of play, a fresh
`KF4_DATA_DIR`. Ryzen 7 5700X, RADV (Mesa 26.2). A real window was not measured (below, step 0).

| run | pictures a second (lowest second) | refreshes not one step of the world | longest gap in play |
|---|---|---|---|
| 000, `--frame-rate monitor` | 169.1 (165) | 0.9% | 23.6 ms |
| 009, `--frame-rate monitor` | 169.8 (168) | 0.1% | 11.8 ms |
| 000, `--frame-rate 240` (the owner's setting) | 221.8 (216), 654 pictures left out | **9.4%** | 16.9 ms |
| 000, monitor, driver's shader cache off | 169.2 (164) | 0.5% | compiles: **7.7 s in all, the longest 1,058 ms** |

What each says:

1. **The pipelines.** The shipped list (`gsn_pipeline_list.h`) was last made on 2026-10-24, before `fast` existed,
   and has no line for `--native-colour high` (tag `k1`). For the owner's settings (`m2.c1.k1.s1.x0`) nothing is
   made ahead: `pipelines made ahead: 224 on the lists, 0 built`. Every pipeline `fast` needs is made by the draw
   that first needs it, with the GS's thread waiting. On RADV with Mesa's own disk cache warm that is 0-2 ms each
   (25 on map 000); with it cold (a new Mesa, a shader changed by a new build, a first run) it is seconds in all,
   and up to a second for one. Every build that changes a shader makes its lines in both lists stale, so the owner,
   who builds often, meets them again and again.
2. **The pipeline cache is written on the GS's thread** (`save_pipeline_cache`, gsn.cpp): 3.5 MB from
   `vkGetPipelineCacheData` and a file written, two seconds after any pipeline is made. A second, smaller hitch
   after each compile.
3. **240 on a 169.83 Hz screen judders by design.** Pictures timed for 240 a second land on 170 refreshes: one in
   ten refreshes shows the world move by two steps or none, and the GPU draws 70 pictures a second nobody sees. The
   owner's `settings.json` has `frame_rate: 240` with vsync off.
4. **The GS's thread works in bursts, and is nearly full.** The frame's packets (gif ~13 ms) and the back end's
   flush come within one vblank: about 20 ms (p99 26, worst 45 ms on 000). That is not all a game frame costs it:
   the recording of the pictures between comes in the next vblank (step 2, measured again). Over 33 ms the
   pictures between are cut (`cut to fewer a vblank`). RENDERER.md, "Where 240 stands", found the same at 009 at
   240.
5. **The refresh clock's latency is taken from the hitches.** The time before each refresh a picture is made
   for is the 98th percentile of the last 256 pictures' lateness: 6-7.5 ms after a clean run, 17 ms after the
   cold one. A compile makes every picture after it a refresh later than it needs to be.
6. **The GPU's time a picture is far above what the scene needs.** 3.46 ms a picture at 4x MSAA 2x
   (`KF4_GSN_GPU_TIME=2`, 000) for about 21,000 triangles. A picture is 13.6 render passes and 11.6 resolves,
   with 221 MB loaded, 233 MB stored, 204 MB resolved and 78 MB copied for blending: about 0.7 GB of memory
   traffic a picture. 156,137 of 193,234 render passes ended for shader blending. By kind: scene draws 0.54 ms,
   passes untagged 0.55, resolves 0.49, skinned groups 0.39, Z-first alpha-tested 0.38, MSAA loads 0.28, scene
   ops 0.26, GS batches 0.20. This is the ceiling of any unlocked frame rate: about 285 a second today.
   **(Corrected 2026-10-29: not at the owner's settings.** The native GS's thread stops the pictures first, at about
   175 a second on map 000: "Where it stands" at the top, and step 3's measures.)
7. **The game's thread hashes about 5 MB of meshes a game frame** (native draw finds its meshes by content: 5.9 GB
   hashed in the run). It keeps up on this CPU (no game vblank over 25 ms in play) but it is the first thing a
   slower CPU or a busier map loses.

## Smooth play: the targets

At the owner's settings, on this machine and on Windows with an NVIDIA card (the work is platform-neutral):

- **No stall from a pipeline, ever**: no gap over two refreshes from a compile in play, including a first run of
  a new build on a cold driver cache.
- **Steady play**: at least 99.5% of refreshes one step of the world, at 170 and 240 Hz, every spot of the map
  tour; no second under the refresh rate.
- **GPU at most 1.5 ms a picture** at 4x MSAA 2x (from 3.46), so that unlocked reaches 500 a second and 240 Hz
  keeps up with room.
- **`--frame-rate unlocked`**, as smooth as `monitor` where the screen can show it (VRR, or tearing allowed).

## Smooth play, step 0: land what is in flight, and measure what the owner sees

- Commit the refresh clock (`refresh_clock.c`, `screen_vblank.c`, `refresh_clock_test`) with its checks.
- **A real window's record.** `KF4_PRESENT_LOG` in a real window, with each picture's present and, where the
  system knows it, the refresh it reached; `pace_test.py --window` reads it. The headless runs above cannot see the
  compositor, the swapchain or fullscreen; this is how the owner's own play gets numbers.
- **`pace_test.py --cold`**: a fresh data folder and the driver's cache off (`MESA_SHADER_CACHE_DISABLE=true`;
  `__GL_SHADER_DISK_CACHE=0` on NVIDIA); prints the compiles made in play, their total and the longest. Step 1's
  check.
- Owner, now: set the frame rate to **monitor** (the Debug page, or `"frame_rate": "monitor"`): it is item 3
  above, and it costs nothing.

## Smooth play, step 1: pipelines never stall play

Why there is building at all: the shaders are compiled ahead, into SPIR-V (`gsn_shaders.h`), but SPIR-V is not
what a GPU runs. The driver turns each pipeline (shaders together with the render pass's formats and samples, the
vertex layout and the state not made dynamic) into its own GPU's machine code, which differs by GPU and by driver
version; a pipeline cache from another GPU or driver is refused by its header. So the machine code cannot be
shipped (Steam's shader pre-caching does it by collecting caches per GPU and driver from players). What can be
done is to make every pipeline before play and keep the driver's result on disk: this step makes that complete.

In order; each is useful alone.

1. **The shipped list made again, for `fast` and every tag the settings offer** (MSAA 1/2/4/8, colour exact and
   high, cut-out AA, the scene's samples), from map tours with `--renderer fast` and `modern`. A ctest checks the
   shipped list against the built shaders: no stale line, every tag the settings page can produce present. A
   shader change that forgets to remake the list then fails the tests, instead of the owner's next play.
2. **Lines not tied to the settings.** A pipeline's shaders and state are the same at MSAA 2 and 4; only its
   render pass differs. The warm-up makes each line again with the render pass of the settings in use
   (attachments' formats and samples rewritten, then checked by Granite's hash as now). One tour then covers every
   combination, and the data folder's lines made at other settings count too.
   **Then the whole set worked out, not found by playing: the pipeline set** (agreed 2026-10-28; built, below).
3. **The pipeline cache and the data folder's list written on a thread of their own**, not the GS's. **Built**:
   the GS's thread only says the files are wanted; `kf4-gs-files` takes the cache's bytes from Granite (about 3 ms
   at 3 MB, 1.2 ms more of it allocating) and writes them (1.3-1.5 ms). Taking the bytes was the larger part, and
   was not safe on any thread: on RADV Granite keeps pipeline binaries (`VK_KHR_pipeline_binary`) and walked their
   maps without their locks, while the back end's thread and the warm-up's could be adding to them. A Granite edit
   made at configure (`cmake/granite_pipeline_warm.cmake`) holds the maps' read locks for the walk. The cache is now
   written while a cold warm-up runs too (every two seconds, as it grows), so a start cut short keeps what was
   built. Checked: TSan over a cold start with the files written seven times while two warm-up threads compiled,
   nothing from this code or Granite's cache; the next start took all 184 pipelines from the file.
4. **A draw in `fast` never waits for a compile.** A pipeline not made is handed to the warm-up's threads, and the
   batch is not drawn until it is (a few pictures with RADV's cache warm; up to a second cold). `modern` and
   `native` are the references and still wait (their pictures are checked byte for byte). **Owner's choice**: an
   object missing for a few pictures, or a stall; this plan proposes the first for `fast` only.
5. **One line, not one a pipeline**: `--stats` and Shift+P's profiler count the compiles made in play and their
   time; the log keeps a line only for those over 5 ms. **Built**: `port_profile_compile` counts them whether the
   panel is open or not (`profile_test`); the profiler's line and `--stats`' `pipelines made ahead` give the count,
   the time in all and the longest.

Check: `pace_test.py --cold` at 000, 009, 063 and 022: no picture gap over two refreshes.

### The pipeline set (`gsn_pipeline_set.inc`)

The start already built pipelines ahead ("Building shaders... N/M"); what it lacked was a complete list, since the
lists are what tours happened to draw, at the settings they ran with. The set is every pipeline the draws can ask
for, worked out from the code for the settings in use.

**Why it is small.** Granite keys a pipeline on its program (vertex and fragment shader), the compatible render
pass (the attachments' formats, the subpass), the vertex layout and the state it bakes in. With dynamic state
(`dynamic_state_choice`) depth, stencil, culling, blending and the write mask are set at each draw and left out of
the key. Decoded, every pipeline in the recorded lists had the same baked state (a triangle list, the dynamic bits)
and one of two vertex layouts. So a pipeline is a program and a pass shape: at the owner's settings the lists held
33 graphics pipelines from 25 programs and 9 pass shapes, and 14-57 at other settings.

**How it works.**
- Each place that draws is an entry of `ps_sites[]`, a function that adds the pipelines it can ask for:
  the program and the pass it draws in (`PsPass`: colour format, depth format, samples, subpass: none,
  `msaa_subpass`'s, `scratch_subpass`'s or `resolve_subpass`'s, the last split out of `begin_pass` for this), the vertex layout (none, `Vtx`, native draw's `Corner`) and
  conservative rasterization; or a compute program. A new kind of draw is a new entry.
- The passes are made from the same functions the draws take them from: `colour_format_of`,
  `sa_depth_format_wanted`, `sa_samples_for` (split out of `sa_switch` and `sa_shape` for this), at the settings
  wanted, since the targets take them only at a vblank after the start. The samples matter more than they look:
  Granite's hash of a compatible render pass leaves them out, so a pipeline made at the wrong samples would be found
  by a draw at the right ones.
- At the start (`pw_start`, from `gsn_pipeline_cache`) the entries are listed, each pipeline's `DeferredPipelineCompile`
  made as a `Cmd`'s draw would have it (Granite's opaque state with the dynamic bits) and its hash worked out with
  Granite's own function, and those not already on a list are added to the warm-up's jobs, made on its threads
  before play. The entries are made in the order the game needs them: the scanout, the copies and the GS's 2D (the
  title) first, the world last.
- **The check:** a pipeline a draw makes in play is counted against the set (`--stats`, "pipeline set"), and with
  `KF4_GSN_WARM=set` (the set alone made ahead, not the lists) each one outside it is logged with its shaders'
  names (`gsn_spv_all` now has them, from `tools/build_shaders.py`) and its pass. Outside the set means an entry is
  missing or names a pass wrong. The set's hashes are also checked against the lists': at the owner's settings all
  35 recorded (33 graphics, 2 compute) are in the set.
- Not covered: a device without blending as dynamic state (no `VK_EXT_extended_dynamic_state3`): the blend
  equation is then in the key, and the lists alone are used, as before. And the upscalers' composite and motion
  passes (`gsn_composite.inc`), `--upscaler` only: not entries yet.

**The entries** (184 pipelines at the owner's settings):

| entry | what | pipelines |
|---|---|---|
| the scanout | `narrow_pass`, the movies' fields, the picture scaled and sharpened: 8-bit, one sample | 4 |
| the full-screen copies | a target's picture and Z into its multisampled images and back, the copy shader blending reads in the pass; the scene's taken from the targets, given back, resampled | 24 |
| the GS's batches | in a target's pass: Z clamped with its Z buffer, the plain program without; Z only; cut-outs at every sample; shader blending from the scratch; multisampled, resolving the copy the next run blends from; native draw's screen passes and trail (2D) over the plain programs; without the scene, the models' own vertex programs (`nd_program_of`: made whether native draw is on yet or not) | 40 |
| the replay | the pictures between's compute programs | 2 |
| the GS's draws in the scene | `sa_draw_range`'s PS2 corners and the water, in the scene's pass | 36 |
| the scene's draws | its batches, skinned groups, the PC world's materials, the ties' Z-first draws, shader blending in the pass and its copies, in the scene's pass at its samples and at one, with the scratch or not | 78 |

**Measured** (map 000, the owner's settings, 30 s of play, a fresh data folder each run):

| | before (the lists) | the set |
|---|---|---|
| driver's cache warm: pipelines made in play | 25 | **0** (all from the cache in 0.01 s) |
| driver's cache cold (`MESA_SHADER_CACHE_DISABLE=true`): made in play | 25 compiles, 7.7 s, the longest 1,058 ms | **0** in play (1 by the title before play, 3.9 s) |
| cold: made ahead | 0 | 177 in 36 s on 8 threads; play waited 13.7 s |
| cold: refreshes not one step, the longest gap | 0.5%, a 4-refresh jump 6 times | 0.7%, 25.6 ms (as with the cache warm) |

(The cold rows are of the set at 177, before the resolve passes' 7 were added.)

**Coverage** (`KF4_GSN_WARM=set`, a fresh data folder, 20 s of play after a teleport): pipelines made in play
outside the set.

| settings | map | the set | outside it |
|---|---|---|---|
| the owner's (`fast`, 4x, MSAA 2, colour high, 169.83 Hz monitor) | 000, 009, 063 | 184 | 0 |
| `fast`, 3x, MSAA 8, colour high, 240 | 022 | 177 | 0 |
| `modern`, 4x, MSAA 1 | 000 | 73 | 0 |
| `native`, 3x, MSAA 4 | 000 | 129 | 0 (3 before `nd_program_of` and the resolve pass) |

ASan and UBSan (`out/linux-clang-asan`, the owner's settings, map 000, the set alone): nothing from this code (the
known underflow in `SoundDriver_Remote`; Granite's `memcpy` of no bytes from null for any pass without colour, the
game's own too).

**The over-build is accepted** (the owner, 2026-10-28: comprehensive over small). The set is a superset, 184 at the
owner's settings against 35 recorded there; a cold start pays for it once per driver or shader change (36 s on 8
threads, play waiting for what the intro and title have not hidden). An entry is narrowed only where the code shows
a draw cannot reach a pipeline, never by what a tour happened not to draw.

## Smooth play, step 2: the pictures between off the GS's thread

### Measured again (smooth/picture-pump: step 1 and the refresh clock, 2026-10-28)

Map 000, the owner's settings, `KF4_PRESENT_SIM=169.83`, `--frame-rate monitor`, `KF4_HITCH_MS=1`, vblanks 2000 to
3600. The machine was busy (a browser playing video, load 2-4), and the game itself was measured as before.

| a game frame on the GS's thread | ms |
|---|---|
| the frame's packets (gif) | 15.2 |
| the vblank (mostly the flush) | 7.6 |
| the recording: the frame's packets drawn a second time, for the first picture between | 7.1 (4.55 ms each in `--stats`) |
| a replay of the recording | 0.0006 a picture |
| **in all** (median, p90, p99, worst) | **31.2**, 34.7, 36.4, 38.0 of 33 |

In play: 160.4 pictures a second (lowest second 154) and 8.4% of refreshes not one step of the world, against
169.1 and 0.9% in the table above. The difference is the load, which is the finding: the GS's thread has about 2
ms to spare a game frame, so anything else on the machine takes pictures between. The pictures left out are
mostly in the vblank of the burst.

What this changes. The plan below (2b) has the back end make the pictures between, so that none waits behind the
GS's thread's next packets. But a replay already costs nothing; what the late pictures wait for is the recording,
made on the GS's thread after the frame's burst (23-27 ms, then 7 ms more). The pump cannot make a picture before
its recording is there, so on its own it would not take this lateness away. So step 2 is now in two parts, the
recording first (agreed by the owner, 2026-10-28).

### 2a: the recording made from the frame's own drawing

The frame is drawn from its packets once into the buffer not shown, and then once more, rewritten by
frame_interp.c, as the first picture between: recorded as it is drawn (Cmd, Recording), and its kicks set up to be
moved (InterpKick, gs_replay.vert). Instead, the frame's own drawing is recorded, and its replays made into the
buffer shown. That takes about 7 ms of 31 off the GS's thread each game frame, and the pictures between can start
as soon as the frame is drawn.

What the second drawing does differently, and so what the replay must do instead (to be checked first, before a
header):
- Its draws go into the buffer shown (FRAME), and TEX0 and BITBLTBUF where they read the frame point there too:
  at a replay, the frame's images swapped for the shown buffer's, as the replay already swaps the targets' images
  (`ReplayJob::images`, `now_image`).
- Local-to-local transfers into anything but the frame are not started, but for the copies of the frame it reads
  after them and the map change's COPY_RESTORE; local-to-host ones neither; FINISH, SIGNAL and LABEL are not sent.
  The frame's drawing has them: each kept out of the recording (marked as the GS records it), or made again as a
  copy of the swapped images.
- Its vertices are drawn by gs_replay.vert from kicks, the frame's by the usual programs. Either the frame's draws
  record their kicks too (and its own picture is then drawn as now, its pixels the same), or the recording keeps
  both programs. The game's own frames must stay the same pixels with the frame rate above 30 or not (frame_test
  `--dump-frames` of the even vblanks, as now).
- The recording is checked against the GS's state before it (`interp_cannot_replay`: targets, registers, VRAM):
  for the frame's own drawing that state is the one before the frame, which the GS has again only if nothing
  between drew differently (`port_interp_end_vblank`'s exact restore).

If the frame's drawing cannot be recorded for a frame (a read-back, a cut), the picture is drawn from the packets
and recorded as now.

**Checked (2026-10-28): not as written.** Two things stop it:
- **The moves come after the frame is drawn.** frame_interp.c makes the frame's `GsnInterpMove`s at its first
  picture between (`port_interp_picture` → `gpu_frame`), after the vblank, from the whole frame and the one
  before (`objects_pair` pairs both ways). The GS draws the frame's packets as they come, before that. But the
  recording's draws are decided from the moves (`interp_moving`, `interp_place_at_0`, `interp_alpha_range`,
  `interp_t_at_0`: the scissor, the blending's copies at t 0, the alphas, the texture rows, the Z). Recording the
  frame's own drawing means holding its packets until the vblank (losing the overlap with the game's sending) or
  deciding all of those as if anything could move anywhere, in the frame's own drawing too.
- **The recording is not the frame's drawing with its draws' vertices swapped.** 89 places in the GS decide
  otherwise while recording (`interp.active`: native draw's six paths, the scene's targets, the copies, the
  corners made on the GPU, the PC world). Each would have to make both, for the frame and for its pictures.

### 2a, instead: the recording made cheaper (the owner's choice, 2026-10-28)

The same recording, the same pictures, less of the GS's thread.

**Built: a frame recorded once** (`interp_settle_newer`, gsn.cpp). A frame's recording that began with the frame
buffer's multisampled image newer and ended with its 2D drawn into the image found the GS "not as recorded" at its
next picture (`--stats`: "drawn from the packets again ... (targets)") and was recorded again: 367 of the first 1,400
frames of tests/newgame.pad, 4 ms each. Now the two images are made the same first (a resolve or a load, exact) and
named newer as before the recording. In play on map 000 the frames were already recorded once (none of the 367 is
after the teleport); at the new game's start, a quarter of the frames lose a second recording. `--stats`: "with a
multisampled target's images made the same first". Checked: 1,017 pictures (every 7th vblank to 2,420 at 170, every
picture of it, 299 of them right after a settle) byte for byte the old build's; ASan and UBSan over the same run,
nothing from it.

**Where a recording's 4.2-4.5 ms goes** (map 000 in play, the owner's settings, `--fast`, 170; `--stats`' timers
over the recordings, and 400 samples of `kf4-gs`'s stack with `eu-stack`, 125 of them in a recording):

| | share of a recording |
|---|---|
| native draw's draws (`nd_register`) | 74% |
| of it, the skinned models (`nd280_draw`): each batch's A+D writes, which end the batch before (`gs_write` → `flush_batch`) | 28% |
| the batches drawn (`sa_draw_batch`, `fast`'s), from native draw's and the GS's | 31% |
| the uploads asked for again (`replay_upload`, `sa_upload_replayed`) | 13% |
| native draw's own planning (`ns_nd_plan`) | 1% |

So what is left to cut is in native draw's draws as a picture between makes them, chiefly the skinned models'
batches: a picture's corners already come from the frame's (`nd_prep`: "taken from the frame's draws"), but each
batch's state is still written and flushed through the GS one batch at a time.

**What the pictures wait for** (2026-10-29; map 000, the owner's settings, 169.83 Hz, real time; a diagnostic build
timing each picture from when the controller sends it to when the GS's thread takes it):

| | pictures | wait: median, p90, p99 |
|---|---|---|
| replays in the vblank of the recording | 3,064 | 10.75, 12.5, 14.2 ms |
| replays in the vblank after | 4,715 | 0.6, 4.8, 7.3 ms (none behind the next frame's packets) |
| the recordings themselves | 1,666 | 4.7, 5.9, 7.3 ms, then 5.8 ms to record |
| the exact restore (571 of 1,666 frames) | | 0.76, 4.3, 4.6 ms of the GS's thread |

So a picture between waits for its recording, not for the GS's thread's next packets: the pump (2b) would make the
same pictures at the same times. In the present log this is the steady part of the uneven refreshes: a gap of 4
refreshes (23.6 ms) at the start of a recording's vblank, 1-3 a second.

Sampled in real time (`eu-stack`, both threads): the GS's thread 12% waiting at the vblank for the back end
(`backend_send`), the back end 28% waiting for the GPU (`next_frame_context`: Granite's frame contexts are taken a
picture each, pictures between too, so 4 is 4 pictures in flight), 18% making calls, 51% idle. 8 frame contexts
instead of 4: no difference (10 runs of 25-40 s, within the runs' spread). What is left of a recording is flat:
native draw's batches drawn through `sa_draw_batch`, no line over 2% of the thread.

Also: runs where the game itself falls behind for a second or so (55-58 game vblanks a second, gaps of 77-117 ms),
in about a third of the runs, before this change as after it, with no hitch on the GS's or the game's thread; the
desktop was busy (kwin, plasmashell, a browser playing video). Not found yet. And pace_test.py counted the last
vblank's pictures, which the run writes out as PNGs (0.4 s): that was every run's lowest second (about 114); fixed.

| map 000, 25 s, 169.83 Hz | uneven refreshes a second | pictures a second (lowest second) |
|---|---|---|
| before (d4919d86), three runs | 4.16, 8.76, 1.80 | 168.3 (142), 162.6 (110), 168.7 (150) |
| after (a frame recorded once), three runs | 1.64, 1.12, 1.00 | 169.0 (159), 169.2 (165), 169.3 (164) |

(Alternating runs; the spread between runs is as large as the difference.)

**So, next:** the pump is not built (it buys nothing while the recording is what pictures wait for). What would move
the pictures are less of the GS's thread before and in the recording, and the GPU's time a picture (step 4), which
is what the back end, and through it the GS's thread, waits for at the vblank.

### 2b: what it needs (checked 2026-10-28)

A replay needs more of the GS's than the recording and the phase: it draws into the GS's own targets (the buffer
shown, the scrolling textures' targets, the reflection), found as the GS has them at that point of its stream
(`interp_cannot_replay`'s `replay_images`), and the buffer shown is the one the next game frame draws into once
the game swaps. So the back end cannot make a picture whenever one is due while the GS's thread works through the
next frame's packets: it would draw into the targets that frame is drawing. The pump needs the pictures between
drawn into images of their own (each target the recording writes, a copy of each it only reads, and its own
picture for the scanout), named through `ReplayJob::images` as now.

Checks: the pictures between the same as today's at each phase (frame_test with `--renderer native` and `fast`,
the map tour's pictures between, `KF4_INTERP_STILL`); the GS's thread's time a game frame (`pace_test.py --mode
cost`, and `KF4_HITCH_MS` in real time as above) before and after.

### 2b: the pictures between, made by the back end (the picture pump)

After 2a. It still matters for step 3 (unlocked), and so that no picture waits behind the GS's thread's next
packets once its recording is made.

Today the controller's thread works out each vblank's pictures between (`port_interp_schedule`, up to 16) and the
GS's thread sends each one's replay. A burst on the GS's thread (item 4) delays or cuts them. The replay is
already one call in the back end's stream (`OP_REPLAY`: the recording, shared, and the picture's buffers), and
costs the GS's thread under a microsecond.

So: the back end's thread makes the pictures between itself, from the newest recording, whenever one is due,
while the GS's thread works through the next game frame's packets. When a picture is due comes from one policy,
asked by the pump (header first, for agreement, per the C/C++ rules):

```c
/* port/include/port/picture_clock.h: when the next picture is due and which moment of the game it shows.
   One implementation a mode: a fixed rate, the screen's refreshes (the refresh clock), unlocked, capped. */
typedef struct {
    double due_s;    /* when the picture is to be handed to the presenter (port_wall_seconds) */
    double world_s;  /* the moment of the game's world it shows (from which the phase between frames) */
} PictureTime;
```

with the GPU's pictures in flight held to two (a timeline semaphore the pump waits on). The GS's thread then only
records game frames, and keeps one game frame's budget (33 ms) on average instead of one vblank's at its worst.
Also: the refresh clock's latency from a measure that a hitch does not move for the next 256 pictures (item 5),
a median with an allowance, decaying.

What to check first: that a replay needs nothing of the GS's thread's but the recording and the phase (the
corners are moved on the GPU, `gs_replay.comp`). Anything else it reads moves into the recording.

Checks: the pictures are the same as today's at each phase (frame_test with `--renderer native` and `fast`, the
map tour's pictures between, `KF4_INTERP_STILL`); the TSan build (`out/linux-clang-tsan`) over a pace run;
`pace_test.py` at 170 and 240: 99.5% one-step refreshes at 000, 009 (its bursts), 063.

## Smooth play, step 3: `--frame-rate unlocked`

As planned, one more policy for the pump (step 2), not a new path:

- **unlocked**: a picture whenever the GPU has room (two in flight), each showing the world at the time it is
  expected on screen (now and the measured latency). The swapchain immediate where tearing is allowed, mailbox
  otherwise (no tearing; only the newest picture before each refresh is shown, for the least latency).
- **a cap** (`--frame-rate-cap N`, and the settings page): unlocked, no faster than N. For VRR screens (FreeSync,
  G-Sync) the cap a few below the screen's rate keeps it in the VRR range.
- The settings page: 30, 60, monitor, unlocked, and the fixed rates under "other"; a fixed rate that is not the
  screen's says it will judder (item 3).

Without VRR and without tearing, a screen shows one picture a refresh, and `monitor` is already the smoothest it
can be; unlocked then gives less latency, not more smoothness. With VRR or tearing it is smoother.

Left as they are, as at every rate above 30: flipbook animations and the HUD's orb change 30 times a second.

### Built (2026-10-29): unlocked as a rate found as it plays

The pump is not built (step 2: it buys nothing while pictures wait for their recording), so unlocked is built on
today's schedule: the pictures between a vblank are still worked out ahead (`port_interp_schedule`, up to 16 a
vblank) and shown at their times, at a rate that follows the renderer.

- `--frame-rate unlocked` (`settings.json` `"frame_rate": "unlocked"`, the Display page's "Unlocked"),
  `--frame-rate-cap N` (`frame_rate_cap`, 0 none; a slider under it), `--tearing` (`tearing`, off).
- **The rate** (`port/include/port/unlocked_rate.h`, `unlocked_rate.c`; `unlocked_rate_test`, ctest
  `unlocked_rate`): between 60 and the cap (or 16 a vblank, 959). At each game vblank gs.c says whether the native
  GS was behind, as the cut judges it, or left pictures between of the vblank before out; and frame_interp.c gives
  the pictures presented a second (the window's half-second measure). Behind two vblanks in a row: down 15% (28%
  when it fell just before). Presented under 90% of the rate asked, 45 vblanks after a change: down to 95% of what
  was presented. Kept up for a second: up 5%, quickly to 95% of the rate it last fell from, past that only after a
  probe's rest that doubles each time a probe falls (to 8 s). The cut of the pictures between judges only at the
  floor. `--stats`: "frame rate unlocked: N a second at the end ..., M times down, K up".
- The second signal is needed: past its limit the renderer leaves pictures out (gs.c's "left out as the renderer
  was behind the game") before its thread is a vblank late, and a fixed 280 at the owner's settings showed 36 of
  the game's vblanks a second on screen without the cut ever judging. Since 2026-11-12 a vblank whose own picture
  is not shown keeps one picture between however far behind (the last of them; `KF4_INTERP_KEEP_ONE=0` as before),
  so no vblank leaves the screen: PERFORMANCE.md, "the game's 57-58 vblanks a second uncapped" (#107).
- **The swapchain** (present_vk.cpp): unlocked is mailbox whatever VSync says (the owner's choice), FIFO where
  there is no mailbox; with `--tearing` immediate, then mailbox. SDL_Renderer's presenter keeps VSync as set.

**Measured** (map 000, the owner's settings, `pace_test.py --mode pace`, 169.83 Hz simulated screen, real time;
`--renderer fast --native-msaa 2 --native-colour high`):

| | pictures a second (lowest second) | game vblanks shown a second | uneven refreshes a second |
|---|---|---|---|
| monitor | 159.0 (153) | 56.5 | 15.5 |
| fixed 240 | 188.8 (183); 1,353 pictures left out | 60.7 | 40.8 |
| fixed 280 | 147.4 (141) | **36.5** | 77.8 |
| unlocked, first (behind alone) | 155.8, 154.0 | **48.7, 49.0** | 73 |
| unlocked, built (with the pictures presented) | 155.1 (147), 155.3 (141) | 58.3, 59.0 | 20.2, 18.4 |

At the owner's settings the rate settles about 175 (7 falls, 33 rises in 25 s). What holds it there is not the GPU
(3.1 ms a picture: room for 320) but the GS's thread: at fixed 240, the pictures left out are left out for it (the
recording's vblank, step 2's finding). So at these settings on this machine unlocked gives what monitor gives, and
more only where the renderer has room: at `--native-scale 2 --native-msaa 1` (1.73 ms a picture) it reached 285 a
second and was still climbing. A real window with mailbox is not measured yet (step 0's window record).

## Smooth play, step 4: the GPU's time a picture to 1.5 ms

**Premise corrected (2026-10-29).** This step was planned on item 6's "the ceiling of any unlocked frame rate". At
the owner's settings it is not the ceiling: the native GS's thread is (step 3's measures). The GPU's time matters
again once the thread has room; until then a GPU at 1.5 ms gives the same pictures.

The world in `fast` is 21,000 triangles a picture; what costs is the passes around them (item 6). In order of
what it saves:

1. **Shader blending without ending the pass.** 156,137 of 193,234 passes end for it. First a count by blend
   equation (`--stats`); then the equations the world uses mapped onto Vulkan's blending where `fast` may differ
   a little (dual-source blending covers most of `(A - B) * C + D`; reverse subtract the rest), the remainder
   drawn in the pass from an input attachment as the runs are now.
2. **One scene pass a picture.** The scene's draws, Z-first draws, skinned groups and shadows in one
   multisampled pass with one resolve; the GS's 2D and screen passes after it, at the window's pixels. This takes
   the MSAA loads (0.28 ms) and most resolves (0.49 ms).
3. **The Z-first alpha-tested draws** (0.38 ms): alpha to coverage at MSAA, or the depth test alone where the
   texture's alpha is only 0 and 1.
4. **The back end's time a draw** (`radv_CmdDraw` on RADV, PS epilogs from dynamic blending): batches of one
   material drawn with one indirect draw; the pictures between's command buffers kept and submitted again with
   only their phase changed.

Checks: RENDERER.md's (gsn_draw_test, the map tour with the scanouts, frame_test `--renderer native`), and
`fast` against `modern` by how much it differs at every tour spot. `KF4_GSN_GPU_TIME=2` before and after each.

### Item 1, built (2026-10-29): the copies in the render pass, in `fast`

Why the passes ended: not the blend equations. Shader blending already copies the destination inside the render
pass for 8-bit single-sampled targets (`COPY_IN_PASS`, the scene's `in_pass`); a multisampled target's copy was a
resolve between passes, and `--native-colour high`'s 16-bit targets (the owner's) were turned away from the
in-pass copy in both paths (`!wide(...)`): every shader-blended run ended the pass and resolved. The in-pass
copies' shaders already handle 16-bit samples (`pass_resolve.frag`'s scale 65535; the scene's run copies took a
fixed 255, now the target's). So `fast` takes `KF4_GSN_MSAA_BLEND=fast` by default (`msaa_blend_in_pass`, gsn.cpp)
and for 16-bit targets too; `KF4_GSN_MSAA_BLEND=exact` gives the resolves back, in `fast` too. `native` and
`modern` are unchanged (their pictures are references).

| map 000, the owner's settings, `KF4_GSN_GPU_TIME=1` | before (`exact`) | after |
|---|---|---|
| GPU a picture (two runs) | 3.418, 3.425 ms | **3.329, 3.335 ms** |
| render passes, resolves a picture | 12.0, 10.2 | 3.1, 0.5 |
| loaded, stored, resolved by passes a picture | 212, 231, 180 MB | 144, 162, 65 MB |

The pictures: map 009:2 (the pond's creatures, the most shader blending), 64 pictures at 120 with every picture
between drawn, 1280 x 600 presented: 82 pixels of 41 million differ, by 1 at most. Pipelines: none made in play
(the set has the scratch passes at every format already).

A pass a run cost less than item 6 implied: 2.6%. Where the rest goes (`KF4_GSN_GPU_TIME=2`, which serializes the
GPU at each mark, so as shares of its 3.33 ms): scene draws 0.55, passes' untagged time 0.50 (10 draws a picture:
the full-screen work at the start of a pass), Z-first alpha-tested 0.35, skinned groups 0.35, GS batches 0.31, the
scene's ops 0.28, MSAA loads 0.27, the in-pass copies of pictures between 0.21, resolves 0.18, copies 0.14. More
than half is full-screen work at 3840 x 1792, two samples, 16 bits a channel, not the 21,000 triangles.

### The other items, measured (2026-10-29)

- **The scene's ops** (the game's depth clear and alpha set, drawn as sprites over the whole scene): left out
  (a diagnostic build, not kept), the GPU's time a picture did not change for the depth clear and fell 0.04 ms for
  the alpha set. Not worth a clear in the pass.
- **Items 2-4 are not built.** Each is a change to how the scene's passes are made (item 2), to the GS's alpha test
  where the colour pass depends on it (item 3), or to the back end's stream (item 4), several days each with the
  checks above; and the measures above say they would not move what the owner sees now: at the owner's settings
  the GS's thread, not the GPU, holds the pictures at about 175 a second (step 3), so a GPU at 1.5 ms would give
  the same pictures until the recording's cost (step 2) comes down.

## Smooth play, step 5: stalls outside the renderer

1. **Map changes.** Measure door and area changes (`KF4_HITCH_MS=25`, `[hitch] game vblank`: disc reads, meshes
   parsed and hashed) at a dozen MAP:PLACE spots; then read the disc image ahead into memory, and parse and hash
   new meshes on a worker thread while the game's own load runs.
2. **The meshes hashed each game frame** (item 7): know an unchanged mesh by where it is and VRAM's write
   tracking, hashing only what changed.
3. **The first picture of each game frame** is drawn from the packets (4.8 ms of the GS's thread each, 2,033 in
   the 000 run; 367 drawn again as the GS was not as recorded): after step 2, measure whether it still matters.

## Smooth play: the order it was planned in, and the owner's choices

Step 0 and step 1's 1-3 first: they are small and take the stalls the owner sees now. Then step 2 (the larger
change: 2a, the recording from the frame's own drawing, then 2b, the pump), step 3 on it, step 4, step 5. Each step lands with its checks, `KF4_DATA_DIR` a temporary folder in
every run, and its numbers here.

For the owner:
- Step 1.4: a pipeline not made yet in `fast`, the batch left out for a few pictures (proposed) or the stall.
- Step 3: unlocked's default without VRR is mailbox (no tearing; **the owner's choice**, 2026-10-28); immediate
  stays an option. Whether fixed rates leave the settings page's main list is still open.
- The targets above.

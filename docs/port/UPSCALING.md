# Upscaling: FSR and DLSS

Drawing the world below the window's resolution and bringing it up with AMD's FSR or NVIDIA's DLSS
(`--upscaler bilinear|fsr3|dlss`, branch `upscale/scene-size`: the sections marked done below). It follows
RENDERER.md's rules: the PS2's picture stays the default, and this is one more opt-in switch.

## What it buys, and what it does not

- **Mostly picture quality, some speed.** A temporal upscaler is also an antialiaser. It would take over from MSAA
  and `--native-cutout-aa`, and would smooth what those miss: draws without Z, far fences and grass, and
  shimmering texture edges.
- **The speed gain is smaller than in a modern game.** Most of a picture's GPU time does not scale with its pixels.
  At the pond, 1x without MSAA still took 2.75 ms against 4.26 ms at 3x with MSAA 2 (NATIVE_DRAW_P8.md, "GPU cost
  at the owner's spots"). The upscaler's own pass runs once for every picture shown, pictures between included.
  At 240 Hz a 0.5-1 ms pass is 12-24% of the GPU on its own. So the win is a sharper 1440p or 4K picture for the
  cost of today's 2x, not more pictures a second than today's 2x gives.
- **No frame generation.** The port's pictures between (`--frame-rate` up to 240) already move every model and the
  camera to the picture's time, from the game's own data. DLSS-FG and FSR 3's frame interpolation guess the same
  thing from the picture, and they add latency and artefacts. Only the super-resolution parts are wanted.
- **The modern renderer only** (`--renderer modern`). Upscalers need per-pixel depth and motion. Only the scene
  (`gsn_standalone.inc`) has real depth and knows where each vertex came from. The PS2 path and hosted mode draw
  through the GS's integer Z, so they have neither.

## Where it goes: in the composite, before the 2D

T2 already splits the picture the right way (NATIVE_DRAW_P8.md, T2; `gsn_composite.inc`). The scene, meaning the
world, is drawn at a scale and sample count of its own. The GS draws the HUD, messages, menus and fades at its own
scale. `sa_composite` makes the picture shown from both at the scanout, once a picture, in the order pictures are
shown (which the temporal upscalers need: "The upscale slot" below). The upscaler goes there, between the scene's
picture kept for the composite (`SaHeld::hi`) and the composite's own work (`sa_upscale_held`):

```
scene (jittered, render size)  --colour, depth, motion-->  upscaler  -->  up, at the GS's scale  -->  sa_composite
GS 2D (its own scale)  ----------------------------------------------------------------------->  sa_composite
```

So the HUD and text are never upscaled, and the GS's 2D arithmetic is unchanged.

## The inputs, and where each comes from

| input | now | still to do |
|---|---|---|
| colour at the render size | the held picture (`SaHeld::hi`), at a whole-number scale below the GS's | any size: the scene's size free of the GS's integer scale (below) |
| depth | the scene's is D32S8 (the shadows' stencil), so the motion pass writes an R32F of `near / the view's depth` (1 near, 0 far, inverted and infinite), with near and the field of view from the screen matrix ("FSR 3.1") | |
| sub-pixel jitter | done ("Jitter"): `ndc` offset in the scene's vertex shaders, Halton(2,3), FSR's count of steps | the GS's own 3D, the trail and named targets are not jittered |
| motion vectors | done for the camera ("Motion vectors"): at the composite, from depth and the two pictures' cameras | models' own motion: each vertex shader projecting its vertex at this picture's time and the last's (`nd_between.h`, `between_move`), a second colour target in every scene pipeline |
| reactive / transparency mask | none | effects (program 0x700, additive), the water's surface and shader-blended runs, flagged by kind as the scene draws them. They have no reliable motion of their own, so they should not leave trails |
| exposure | LDR: the GS's 8-bit colour as it is (FSR's non-linear colour flags; DLSS not HDR) | |

Motion across a frame boundary: a picture between at time t of the pair (frame N-1, frame N) is followed by
pictures of the pair (N, N+1). The camera's motion is worked out from the last picture composited, whichever pair it
was of, so it is exact there. Models' own motion, when it comes, would need the previous pair's `Between` data for
one more picture.

## The upscale slot (done, branch `upscale/scene-size`)

`--upscaler bilinear` with `--renderer modern` and a `--scene-scale` below the GS's. The scene kept for the
composite (`sa_hold`) is brought up to the GS's scale at the composite (`sa_upscale_held`, a linear blit for now), and
the picture shown is made at the GS's scale (`sa_held_upscaled`). The GS's 2D, fades and passes stay its own, at its
scale, and the world is the upscaler's. Without the upscaler, a scene below the GS's scale goes through the composite
at the scene's scale, which brings the 2D down to it too: the HUD at 2x under a 4x GS.

**Pictures are drawn in another order than they are shown.** The GS draws a game frame's own picture first, then the
pictures between that are shown before it (they go from the frame before toward it: STEP3.md, "Higher frame
rates"). At 240: the frame's own picture is drawn after vblank 2k - 1 and shown at 2k + 2, while the pictures
between toward it are drawn and shown at 2k and 2k + 1. So anything temporal (the upscalers' history, the motion
from the last picture) has to work in the order pictures are shown. That is the composite's order: it runs at each
picture's scanout, once a picture (4,966 composites for 4,966 pictures in a 240 run), the frame's own picture kept
until then (`sa_hold`, at most 3 vblanks). `sa_give`, where the scene hands its picture to the GS, runs in the order
pictures are drawn. A temporal upscaler there would see the camera jump a frame ahead at every game frame and then
back.
- **Whole-number scales, for now.** On a 4K screen the GS is at 4x (3840 x 1792 with native widescreen), and the
  scene at 2x or 3x. With native widescreen the ratios differ across and down at 3x (4 to 6 across, 3 to 4 down:
  1.5 and 1.33), and are the same at 2x (2.0 both). The FSR 3.1 dispatch takes the render and output sizes
  apart. Whether it, and DLSS, take unequal ratios is not settled from their docs yet. If not, the scene's size
  below is needed for 3x, or the output stops at one ratio and the presenter takes the rest.
- **The temporal upscalers go in `sa_upscale_held`.** The held picture needs, besides its colour: its depth (a copy
  at `sa_hold`, as the colour is), its camera, and its jitter. A picture between's recording holds once, and its
  replays make the hold's copies again but not the GS thread's work. So a picture between's camera and jitter are
  the GS thread's at its composite (the picture just replayed), and a frame's own are kept with its hold. `sa_hold`
  runs at the release only, not at a turn in the middle of a frame, so the upscaler runs once a picture.
- Checks: with `--upscaler off`, every picture of 6 vblanks on 000 and 009, at the GS's scale and at `--scene-scale
  2` (the composite), is byte for byte the build before. With it on, `--scene-scale 2` under `--native-scale 4`: 3840
  x 1792 pictures, the HUD the GS's at 4x. The world is a bilinear 2x, and turning with the d-pad the pictures
  between follow native's motion picture for picture.

## Jitter (done, branch `upscale/scene-size`)

Each picture shown takes the next step of the Halton (2, 3) sequence: an offset within half a scene
pixel each way, added in NDC to the positions of the scene's draws into its frame (`sa_000`, `sa_280`, `sa_700`,
`sa_water`, built a second time with `ND_JITTER` as `sa_*_jitter.vert`: the Picture's `counts.zw`, where
`GSN_M_JITTER` is set). A picture between has its own in its Picture (`InterpPictureHeader::jitter`, the two words
after the kicks and runs that were padding, `sa_jitter_picture`), so the recording's replays each draw at their own. The frame's
own picture binds a Picture of its own (`sa_jitter_frame`, at the scene's clear) instead of the buffer of zeros. On
with a temporal upscaler, or `KF4_UPSCALE_JITTER=1` to look at it. The sequence begins again after FSR's count of
steps: 8 times the square of the scale's ratio across, 8 at least (32 for the scene at 2x under a 4x GS).
- Not jittered: draws into a named target (the reflection, the scrolling textures: textures the frame reads), the
  trail strip, and the GS's own 3D the scene draws (`gs.vert`, a few in a hundred of the scene's batches). They stay
  still under the upscaler's jitter.
- With jitter off nothing compiles differently: the jitter's code and the Picture's member are only in the
  `sa_*_jitter.vert` builds (`ND_JITTER`, `tools/build_shaders.py`), chosen by `sa_programs` and
  `sat_water_program` when `sa_jitter_on()`, and the Picture keeps its layout (the jitter in `counts.zw`), so every
  other shader is the SPIR-V it was word for word and the shipped pipeline list (`gsn_pipeline_list.h`) and the
  pipeline caches stay valid. (A first version added a `vec4 jitter` to the Picture and the code to every scene
  shader: their SPIR-V changed with the upscaler off, the shipped list's lines for them went stale and 11-14
  pipelines a spot were compiled by draws again at a fresh data folder.) With jitter off, every picture of 6
  vblanks on 000 and 009 (the pond's water too), and on 000 at `--scene-scale 2`, is byte for byte the build before.
- With it on, standing still at `--scene-scale 2 --upscaler bilinear`, each picture differs from the one before by
  0.65-1.07 levels on average where it was 0.06-0.19, and the HUD not.
- With jitter on (FSR, DLSS), the `sa_*_jitter` pipelines are not on the shipped list (no shipped tag has an
  upscaler): the data folder's list has them after the first run.

## Motion vectors (done, branch `upscale/scene-size`)

At the composite, in the order pictures are shown, a held picture's motion from the last picture composited is
worked out a scene pixel at a time (`sa_motion_pass`, `shaders/sa_motion.frag`, RG16F in scene pixels: where the
pixel's surface was in the last picture, less where it is).
- **From depth and cameras, one matrix.** The pixel's place in the GS's coordinates (its middle, the picture's jitter
  taken out) and its Z (the scene's depth over its Z scale) go back through this picture's world_screen and forward
  through the last one's: `M = last * this^-1`, made in doubles on the CPU, one multiply and a divide by w a pixel.
  A depth of 0 (the far plane: the sky, the clear) takes the Z the view's depth goes to at infinity, so in
  homogeneous form it moves as a direction.
- **The cameras.** A game frame's own: the world's camera (mode 0, projection 0) as native draw's records give it
  during the frame's draws (`sa_camera_seen`), kept with its hold. A picture between's: the frame's view moved by the
  picture's move, then the frame's view to screen (`sa_camera_picture`, at `interp_picture_now`), the GS thread's at
  that picture's composite. The depth is copied with the colour at `sa_hold`, so a picture between's replays copy their own.
- **The camera's motion only.** A model that moved by itself (an enemy walking, the arm) is given the camera's.
  Upscalers ghost behind those until their own motion is written by the vertex shaders, which would mean a second
  colour target in every scene pipeline. Their reactive masks are the help meanwhile.
- `KF4_UPSCALE_SHOW=motion` draws a picture of the motion in the scene's place: grey where nothing moved, red and
  green the motion across and down. It goes through the game's brightness like the scene, so read it by eye.
- **Checks.** Standing still the field is even (no motion). Turning with the d-pad in real time at 240, the motion at
  the picture's middle is +1.396 GS pixels a picture (4.19 scene pixels at `--scene-scale 2` with native widescreen,
  3 across a GS pixel), for the frames' own pictures and the pictures between alike. The pictures themselves move 8
  output pixels (4 scene pixels, to a pixel) from one to the next. The world_screen's translation falls smoothly
  across both kinds of picture in the order shown. Off, every picture is byte for byte the build before.

## FSR 3.1 (done, branch `upscale/scene-size`)

`--upscaler fsr3`: AMD's FSR 3.1 upscaler through the FidelityFX API, in `sa_upscale_held` in place of the blit.
- **The library.** The API's headers are vendored (`third_party/ffx-api`, MIT). The upscaler itself is AMD's signed
  `amd_fidelityfx_vk.dll` (the SDK's `PrebuiltSignedDLL`, 9 MB), not kept in the repository: CMake's `KF4_FFX_DLL`
  names one to copy beside kf4.exe, and `KF4_FFX_DLL` in the environment names one to load instead. It is loaded at
  run time by its whole path (`upscale_ffx.cpp`, which keeps `windows.h` and the FFX headers out of gsn.cpp).
  Without it, or where FSR cannot be had (16-bit colours, `--native-colour high`: FFX has no 16-bit UNORM), the
  upscaler says so once and is bilinear from then on, unjittered.
- **On the back end's thread.** The composite sends each picture to upscale as one call in the stream
  (`OP_UPSCALE`, `Cmd::upscale`), with full barriers either side. The back end makes it into its command buffer
  (`gsn_upscale.inc`): FSR records its own compute work there past Granite. The composite's render pass begins right
  after, and Granite forgets what it had bound at a render pass's beginning, so nothing it keeps is wrong after. FSR
  is told every image is in `GENERAL` (its `UNORDERED_ACCESS` state), as all the native GS's are.
- **One context** for the stream of pictures in the order shown (both of the game's buffers), made at the first
  picture and again when a size changes. The one before is freed eight of Granite's frames later, when the GPU is done
  with it. FSR's history begins again (reset) at a new context, or after more than 4 pictures without an upscale (a
  menu's), and the motion with it.
- **Inputs.** Colour: the held picture (the PS2's perceptual colours: `NON_LINEAR_COLORSPACE`, sRGB). Motion: the
  motion pass's (scene pixels, so the scale is 1, 1; no jitter in it). Jitter: the picture's, in scene pixels. Depth:
  the scene's is D32S8 (the shadows' stencil), which FFX's views cannot take, so the motion pass writes a second
  target, R32F, of `near / the view's depth` from the screen matrix's Z and w rows. 1 at the nearest Z the scene
  holds, 0 far away and at the clear: `DEPTH_INVERTED | DEPTH_INFINITE`, the near plane given as `cameraFar` and
  `cameraNear` FLT_MAX as FSR's debug checks want. The field of view is the scene's rows over the screen matrix's
  focal length.
- **The device.** FFX's build asks for `vkGetBufferMemoryRequirements2KHR` by its KHR name. Granite's device has it
  as Vulkan 1.1's own without the extension, so the name gave null and FFX jumped to it. FFX is given a
  `vkGetDeviceProcAddr` that tries the core name where a KHR name is not there.
- **The jitter's sign** is FSR's as given (the offset the scene was drawn at, in pixels, down positive). Of the four
  signs, it is the one closest to native standing still (42.6 dB against 39.4-41.0 for the others).
- **Checks.** `--upscaler off` is byte for byte the build before (000 and 009, 6 vblanks, at `--scene-scale 2` and
  at the GS's scale). On an RTX 4090, the scene at 2x under a 4x GS (1920 x 896 to 3840 x 1792), standing still on
  000 against native 4x: PSNR 42.6 dB (bilinear 42.2), gradient energy 1.81 (bilinear 1.62, native 2.47). Turning:
  47.2 dB (bilinear 46.7), with no ghosting or smearing seen. KF4's textures are coarse, so most of the gain is on
  edges. With `KF4_FFX_DEBUG=1` FSR checks its inputs, and says nothing more.
- **Cost** (`pace_test.py` at 240, real time, `KF4_GSN_GPU_TIME`):

  | | 000 pictures a second | 000 GPU ms a picture | 009 pictures a second | 009 GPU ms a picture |
  |---|---|---|---|---|
  | native 4x | 220.1 (lowest 204) | 4.07 | 238.9 | 3.82 |
  | 2x, FSR | 239.5 (lowest 236) | 3.23 | 239.5 | 3.23 |
  | 2x, bilinear | 239.6 | 2.69 | 239.8 | 2.63 |

  FSR and the motion pass cost about 0.55 ms a picture at 3840 x 1792, once a picture, pictures between too. 000,
  which native 4x cannot keep at 240, holds it.
- **Not yet.** Models' own motion (they move with the camera: ghosting behind a walking enemy shows most), the
  reactive mask for effects and water, sharpening (`enableSharpening`, off), a reset at a cut (teleport, map load:
  today the gap of pictures between catches most), and the scene at 3x under 4x (unequal ratios: 1.5 across, 1.33 down).

## DLSS (done, branch `upscale/scene-size`)

`--upscaler dlss`: NVIDIA's DLSS super resolution through NGX, in the same place as FSR (`OP_UPSCALE`, mode 3), with
the same inputs. Built only where CMake's `KF4_DLSS_SDK` names NVIDIA's DLSS SDK: its licence keeps the SDK (headers
included) out of this repository. The build then copies the SDK's release `nvngx_dlss.dll` beside kf4.exe. Without
the SDK, or where DLSS cannot be had (not an NVIDIA GPU, an old driver, the extensions missing), it says why and
falls back to FSR, which falls back to bilinear.
- **NGX without the SDK's library.** The SDK's way in is a static library that finds the driver's NGX core
  (`_nvngx.dll`, in the driver store, named by `nvlddmkm`'s registry keys) and forwards to it. Its Windows build is
  MSVC's (its C and C++ runtime), which this build (llvm-mingw) cannot link. So `upscale_dlss.cpp` loads the core and
  calls its exports directly, as the library does. The library's Linux build (`libnvsdk_ngx.a`, ELF, readable here)
  shows how: every function forwards its arguments unchanged, except that the core's inits take the SDK's version
  before the feature info (`NVSDK_NGX_VULKAN_Init_ProjectID_Ext(project, engine, engine version, data path,
  instance, GPU, device, GIPA, GDPA, version, feature info)`). Where the core has no extension query
  (`FAIL_NotImplemented`, this driver), the library's own lists stand in. Instance:
  `VK_KHR_get_physical_device_properties2`. Device: `VK_NVX_binary_import`, `VK_NVX_image_view_handle`,
  `VK_KHR_buffer_device_address`, `VK_KHR_push_descriptor`.
- **The parameters' table.** NGX's parameters are an MSVC C++ object with virtual `Set`/`Get` overloads. This
  compiler would lay out its table in declaration order; MSVC groups each name's overloads, last declared first. So
  they are called by slot (Set's `void *` 0, `int` 3, `unsigned` 4, `float` 6, `unsigned long long` 7; Get's 8 on).
  Before anything is made, each kind is set and read back the same, and a name never set is not found. NGX's own
  capability (`SuperSampling.Available`) is read the same way.
- **Extensions at the device's making.** `--upscaler dlss` is set before `gsn_open`. There the instance is made
  with NGX's instance extensions, then the GPU is chosen as Granite would (`GRANITE_VULKAN_DEVICE_INDEX`, or the first
  of the most able kind), then the device with NGX's device extensions for it (`dlss_context`). Any step failing
  makes the context as before, and FSR. Granite already turns on `bufferDeviceAddress` where the GPU has it.
- **The feature.** Made on the back end's thread into its command buffer (`CreateFeature1`), one for the stream
  of pictures, made again when a size changes, the old one released eight frames on. The flags: motion at the
  scene's size (`MVLowRes`), depth inverted (the FSR depth: 1 near, 0 far; DLSS has no infinite flag). The quality
  is the one the ratio is nearest: 2x to 4x is Performance, which this driver gives preset M (the transformer model).
  The presets left to the driver are the SDK library's defaults. The project ID is a GUID of holyland's (an engine
  of its own: `NVSDK_NGX_ENGINE_TYPE_CUSTOM`). NGX's logs go to the temporary folder, and to stderr with
  `KF4_DLSS_DEBUG=1`.
- **The jitter's sign** is the same as FSR's: of the four signs, it is the closest to native standing still (47.6 dB
  against 39.1-43.8 for the others).
- **Checks.** On an RTX 4090, the scene at 2x under a 4x GS (1920 x 896 to 3840 x 1792), against native 4x on 000:
  standing still PSNR 47.6 dB (FSR 42.6, bilinear 42.2), gradient energy 2.18 (FSR 1.81, native 2.47). Turning:
  50.2 dB (FSR 47.2, bilinear 46.7).
- **Cost** (`pace_test.py` at 240, real time, map 000 unless said; `KF4_DLSS_PRESET` forces a preset to weigh them):

  | | pictures a second | GPU ms a picture |
  |---|---|---|
  | 2x, DLSS preset M (the driver's for Performance) | 100.4 (009: 111.6) | 7.37 (009: 7.08) |
  | 2x, DLSS preset L | 101.8 | 7.44 |
  | 2x, DLSS preset J | 112.3 | 6.57 |
  | 2x, DLSS preset K | 118.6 | 6.38 |
  | 2x, DLSS preset E (the last CNN model, deprecated) | 130.3 | 5.74 |
  | 2x, FSR | 239.5 | 3.23 |
  | 2x, bilinear | 239.6 | 2.69 |
  | native 4x | 220.1 | 4.07 |

  DLSS costs 3 to 4.7 ms a picture at 3840 x 1792 on an RTX 4090, against FSR's 0.55. It runs once a picture, and
  every picture between is one, so at 240 it holds 100 to 130 pictures a second: DLSS suits 120 Hz, or a smaller
  output (the GS at 3x), better than 240. The pictures between are what make it costly; upscaling only a game
  frame's own picture and moving the result would be frame generation, which is not this. Preset K, a millisecond
  cheaper, scores the same standing still (47.3 dB) but worse turning (46.3 dB, below FSR's 47.2; M 50.2), so the
  driver's M stays the default.
- **A crash seen once** in eight DLSS pace runs (preset E), in `sa_memo_begin` (native draw's kept batches, the GS's
  thread): a picture between's frame's index unreadable. Not seen again in three more preset E runs, nor in the fast
  runs, nor in three GPU-bound runs of native (the scene at 4x with 8 samples: 22 to 100 pictures a second, pictures
  between cut). Whether NGX or a latent bug of the GPU-bound regime is the cause is not settled.

## Later: the scene's size free of the GS's scale

`--scene-scale` is a whole number, 1 to 4 (`sa.sx`, `sa.sy`, `snt.frame_sx`). An upscaler wants any size, for
example 1707 x 960 under a 2560 x 1440 window (FSR's and DLSS's "quality" ratio, 1.5). The scene's vertex
shaders already work in NDC, so the scene itself can draw at any size. The integer scale shows up only where the
scene meets the GS:
- `sa_give`, the scene's frame handed to the GS at the GS's scale, averages whole blocks of scene pixels. With an
  upscaler it can sample the upscaled picture instead.
- The turns' copies between the scene and the GS's targets: with T2 they happen about once a frame (0.26 a
  picture on map 000).
- Scissor and `flat_2d`, in GS pixels times the scale.

Named targets (the reflection, scrolling textures) stay at the GS's scale. They are textures, not the picture.

## Libraries

- **FSR 3.1 upscaler** (AMD FidelityFX SDK, MIT): done, above, through the API's signed DLL on Windows. On Linux
  the SDK's Vulkan backend would have to be built from source (the DLL is Windows' only).
- **FSR 1** (EASU + RCAS, MIT, two GLSL headers): spatial only, with no jitter or motion. It can sit beside
  `present_scale.frag` and `present_sharpen.frag` (whose sharpening is already CAS-like) as a cheap stopgap,
  though it upscales the HUD with everything else.
- **DLSS super resolution** (NVIDIA DLSS SDK, NGX): done, above, built only with `-DKF4_DLSS_SDK=...`.
  `nvngx_dlss.dll` may ship beside the program under the SDK's licence (with its attribution), or the user supplies
  it. Streamline is the other way in. It hooks Vulkan's creation calls, which suits Granite less well.
- **XeSS** has a Vulkan path too. With an interface shared by all three it is a small addition later.

One interface (`gsn_upscale.inc`) would cover all of them: colour, depth, motion, mask, jitter, render and output
sizes, and a reset when the camera cuts (`pending.cut`, a teleport, a map load, a state load). The choice would
be `--upscaler off|fsr1|fsr3|dlss` with `--upscale-quality`, and the settings and overlay entries beside them.

## Steps

1. **The upscale slot** (done, above): whole-number scene scales below the GS's, brought up by a bilinear blit at
   the composite. The scene at any size (`--scene-size W,H` or a ratio) is for later, if the upscalers want equal
   ratios or finer steps.
2. **Jitter and motion vectors** (done), with a debug view of the motion attachment (`KF4_UPSCALE_SHOW=motion`). Check:
   standing still, the motion is zero everywhere but the effects. Turning, the static map's motion matches the
   camera's. At 240 the motion of a picture between is a quarter of the frame's.
3. **FSR 3.1** (done, above) behind the interface. Still to check: the owner's spots (pond, fortress, canyon, under
   water) at 240 for ghosting behind the zombies and bats, the water and the effects.
4. **DLSS** (done, above), the same interface, built only with the SDK.
5. **Masks and tuning**: reactive masks per kind, sharpening (RCAS, or our CAS), and resets at cuts.

Gameplay is untouched. Every step keeps `--upscaler off` byte for byte as before (the off check, the four spots,
the map tour).

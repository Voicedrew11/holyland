/* The PC program's entry: reads the command line, sets up the host side, then runs the game's own
   main.

     kf4 --disc <image.iso or folder> [options]
       --disc PATH       the user's own disc: an ISO image or the folder it was extracted to
                         (or KF4_DISC in the environment)
       --fast            do not hold the game to 59.94 vblanks a second
       --frames N        stop after N vblanks
       --input FILE      press the buttons a pad script says (port/sdk/libpad.c) instead of SDL's
       --log FILE        write the game's state each vblank, and what happens between ("-": stderr)
       --clock DATE      the disc drive's clock, YYYY-MM-DDTHH:MM:SS (Japan's time), not the host's
       --headless        no SDL at all: no window, and the GS draws only for --dump-frames and
                         GS captures (--gs-capture-at, KF4_DEBUG_AT)
       --no-gs           draw nothing (VIF1 and VU1 still run)
       --renderer NAME   who draws: native (step 3's renderer, port/gs/native/, the default; if
                         it cannot start, paraLLEl-GS), gs (paraLLEl-GS, the reference) or both
                         (side by side, their pictures compared; the window shows gs's,
                         --renderer-shown native shows the other, and --renderer-shown heatmap
                         where they differ); modern: native, with the scene drawn into targets of
                         its own (P8, in work: docs/port/NATIVE_DRAW_P8.md; --native-draw
                         on unless given)
       --scene-scale N, --scene-msaa N
                         with --renderer modern, the scene (the world) at a scale (1 to 4) and samples
                         (1, 2, 4, 8) of its own, the GS's (--native-scale, --native-msaa) for the 2D, the
                         menus and the title; the picture shown made from both (gsn_composite.inc). By
                         default the GS's
       --upscaler off|bilinear|fsr3|dlss
                         with --renderer modern and a --scene-scale below the GS's (single-sampled, both),
                         the scene brought up to the GS's scale at the composite, the 2D the GS's at its
                         own scale (docs/port/UPSCALING.md): bilinear, AMD's FSR 3.1 or NVIDIA's DLSS
                         (temporal: the scene jittered, with its motion; amd_fidelityfx_vk.dll, or
                         nvngx_dlss.dll and an NVIDIA GPU, beside kf4.exe; DLSS falls back to FSR, FSR to
                         bilinear). Off by default
       --native-shine whole|exact
                         with --renderer modern, a skinned model's sphere-mapped shine that is blended
                         in the shader (the ice creatures on map 017): drawn whole (the default, much
                         faster) or exact, in runs of triangles that do not overlap, as the GS blends
                         it where a part folds over its own edge (a few pixels along those edges)
       --presenter NAME  how the picture reaches the window: vulkan (the default: a swapchain on
                         the native GS's device, the picture kept on the GPU;
                         port/platform/present_vk.cpp) or sdl (SDL_Renderer); without the native
                         GS, or if vulkan cannot start, sdl
       --native-scale N  the native GS draws at N times the PS2's resolution (1 to 4), or auto (the
                         default): the multiple of 448 lines nearest the picture's height in the
                         window (1440: 3; 2160: 4), at most 2 on an integrated GPU, 1 with no window
       --native-widescreen, --no-native-widescreen
                         with --widescreen, the native GS draws the 16:9 picture wider still, with
                         square pixels: 4/3 of --native-scale across, rounded up (2, 3, 4, 6); on in
                         a new settings.json, off without one (headless)
       --flicker-filter  the game's flicker filter, as on the PS2: each row of the picture blended
                         half with the row above (port_flicker_filter()); off by default
       --native-msaa N   the native GS draws the 3D with N samples a pixel (1, 2, 4 or 8; 1, none,
                         by default): its edges smoothed
       --native-cutout-aa MODE
                         with --native-msaa, the cut-outs (netting, foliage, fences, grates):
                         ssaa (the default: shaded at every sample, their edges smoothed) or off
                         (shaded once a pixel, as the rest of the 3D)
       --native-2d MODE  at a --native-scale above 1, the 2D's textures (the font, the HUD, the menus'
                         and title's pictures) that the game reads nearest: smooth (the default: read
                         bilinear where they are magnified, so letters have smooth, even edges) or ps2
                         (nearest, at the GS pixel's point: each texel a block, as the PS2 at 1x)
       --native-lod MODE the mipmap level the native GS reads textures at: scale (the default: the
                         GS's level made finer by log2 of --native-scale, the detail of the larger
                         picture; at 1x the PS2's), ps2 (the GS's level, picked for 640x448) or
                         full (level 0, the finest, at every distance)
       --native-aniso N  anisotropic filtering of mipmapped textures: up to N reads (1, off, the
                         default; 2, 4, 8 or 16) along a pixel's footprint on the texture, for floors
                         and walls at a grazing angle (gs_frag.h, sample_texture)
       --native-colour MODE
                         how precisely the native GS keeps colours: exact (the default: the GS's 8
                         bits a channel, rounded as the GS rounds) or high (16 bits a channel, the
                         colours' arithmetic in floats, the picture dithered to 8 bits: smooth
                         gradients in the dark instead of steps)
       --native-layer-z MODE
                         how the native GS keeps and tests the 3D's Z: tolerant (the default: Z's
                         fraction kept, straight seams where surfaces meet, and the layers the game
                         blends over its surfaces Z tested within one Z: no hatching, teeth or lace)
                         or ps2 (the GS's whole numbers)
       --native-draw off|on|KINDS
                         draw these kinds of the game's 3D at the game's level (docs/port/NATIVE_DRAW.md):
                         the patched draw code hands them to the renderer core, and the native GS draws
                         them with a vertex shader doing VU1's maths, instead of VU1 and its packets.
                         on: the kinds that have passed their checks (map, sky, items, objects, picked,
                         entities, arm, gold, effects, shadows, water and passes); KINDS, by name: map, models
                         (objects and picked), skinned (or entities), arm, sky, effects, shadows,
                         reflection, water, passes, items, objects, picked, gold. off is
                         the game as without it, and the default unless settings.json says "native_draw":
                         "on" (the overlay's Renderer section). The overlay's Debug page changes the kinds
                         live
       --native-draw-compare native|ps2
                         with --native-draw, the PS2 path's packets are queued too, and the native GS
                         draws the native draws (native) or the PS2 path's (ps2): two runs alike in all
                         the game does, to compare the pictures
       --native-wave-uvs ps2|gpu|cpu|late
                         with --native-draw map, the wave UVs of the water records: ps2 (the default: the
                         PS2 path's picture, late, or gpu with the test wait), gpu (the frame's offsets
                         added in the shader), cpu (the coordinates the game wrote, the blocks parsed
                         again each frame; gpu's picture) or late (the offsets of the frame after, as the
                         PS2 path's DMA mostly reads them)
       --sky modern|ps2  with --native-draw sky, how the sky is drawn: modern (the default: the dome
                         centred on the eye and behind everything, so what lies beyond it shows) or ps2
                         (at the feet with its real distance, as the PS2 draws it). modern keeps the
                         PS2's where other records are drawn before the sky (maps 000 and 004)
       --test-dma-wait   (tests) each frame's packet chain waited for as soon as it is sent: the PS2
                         path then draws the blocks the game changes as the game drew them
       --vu1 NAME        who runs the VU1 programs (0x000, 0x280, 0x5A0, 0x700; 0x276 passes
                         packets through): hle (the default: computed directly, the interpreter's
                         results to the bit; port/platform/vu1_hle*.c), interp (the cycle-accurate
                         interpreter) or check (both, compared run by run, differences on stderr
                         and counted by --stats)
       --no-deinterlace, --no-progressive, --no-anti-blur, --no-adapt-width, --no-whole-frame
                         turn off one of the ways the PC picture differs from the GS's raw output
       --no-seamless-scroll
                         the scrolling textures (the sky's clouds, the lava, the water) as on the PS2,
                         with a seam where they repeat (port_seamless_scroll())
       --ps2-scanout     turn them all off (and --flicker-filter on)
       --widescreen      the opt-in 16:9 picture: a wider view, the 2D kept at 4:3 (port/platform/gs.c)
       --widescreen-hud-centred
                         the same, with the HUD in the 4:3 middle instead of the 16:9 corner
       --present-scaling sharp|smooth|best
                         how the picture is fitted to the window: nearest, bilinear, or best (the
                         default: the native GS scales it with a cubic, area-weighted when made
                         smaller, on linear light; present_scale.frag)
       --present-sharpen X
                         contrast-adaptive sharpening after the scaling, 0 (off) to 1
       --dump-present WxH
                         --dump-frames writes the picture as a WxH window shows it with the best
                         scaling (and --present-sharpen)
       --frame-rate 30|60|120|144|240|N|monitor
                         the opt-in higher frame rates: pictures between the game's 30 a second
                         in play, each drawn from the game's frame with the camera, the moving
                         models, the HUD, fades and scrolling textures moved to where they are at
                         that time (port/platform/frame_interp.c); monitor: the window's screen's
                         refresh rate; 30, the PS2's, by default
       --draw-distance N the opt-in longer view: the fog and the far plane N times as far (1, the
                         PS2's, by default; 1.5, 2, 4, or max: no distance fog) (game/draw_distance.c)
       --draw-distance-sky ps2|push
                         with --draw-distance, the sky keeps the PS2's fog (the default) or has its
                         fog pushed out as well
       --myus-necklace   the opt-in mod: the effect of Myu's Necklace always, worn or not
       --separate-run    the opt-in mod: run on a key of its own (Run: Left Shift), not the action button
       --extended-stats  the opt-in mod: the extended stats panel and hit log (F6; docs/port/EXTENDED_STATS.md)
       --automap         the opt-in mod: the automap, a map of where you have been (M; docs/port/AUTOMAP.md)
       --automap-cheats  the automap with all its cheats on (the whole map, items, enemies, secrets, hints)
       --weapon-sets     the opt-in mod: a second weapon and shield, swapped in on R (docs/port/WEAPON_SETS.md)
       --show-fps        the frame-rate counter in the picture's top right corner (F3 shows and hides it)
       --invincible      debugging: the player loses no HP and does not die (the overlay's Debug page)
       --cheats LIST     debugging: once in play, spells, equipment, items and/or magic (comma-separated,
                         or all), as the Debug page's buttons do; spells1, spells2 or spells3 for the
                         spells at that level (3 otherwise) (port/include/port/cheats.h)
       --dump-frames N,N write the picture shown at these vblanks as PNGs
       --dump-dir DIR    where --dump-frames writes (the current directory by default)
       --host-dir DIR    where host0: files go (the current directory by default)
       --stats           at exit, print the time the DMA controller's thread spent in VU1, in
                         handing GIF packets to the GS and in vsyncs, and VU1's runs and instructions
       --audio-dump FILE write the sound as a 48 kHz WAV file
       --no-audio        play no sound (it is still made, for --audio-dump)
       --controls NAME   the keyboard and mouse layout: ps2 (the default) or verdite (mouse look,
                         port/sdk/controls.c)
       --memcard DIR     the folder that is the memory card in slot 1 (by default memcard in the
                         data folder, next to settings.json); "none" leaves the slot empty
       --record-input F  write what the pad and keyboard send as a pad script, to replay with --input
                         (the sound then follows the game's clock and mouse look is off, so it replays)
       --state-save-at VBLANK[:SLOT|:FILE],...
                         save a state at these vblanks (port/platform/savestate.c)
       --state-load [VBLANK:]SLOT|FILE
                         load a state before the game starts, or at that vblank
       --quick-save-at VBLANK[:SLOT|:FILE],...
                         take a quick save once in play from these vblanks (port/platform/quicksave.c)
       --quick-load [VBLANK:]SLOT|FILE,...
                         load a quick save on the title screen, or in play once the run reaches VBLANK;
                         several, in vblank order, are loaded one after another
       --teleport [VBLANK:]MAP[:PLACE]
                         once in play (from VBLANK), put the player in that map at its PLACE-th place
                         (0, the map's start, when left out; VBLANK:MAP:PLACE with a vblank), keeping
                         the game's progress, as the overlay's Debug page does (port/platform/teleport.c)
       --gs-capture-at VBLANK[:N],...
                         write a GS frame capture of N vblanks (2 when left out) from these vblanks,
                         for kf4-gsreplay (port/debug/gs_capture.c; F9 does the same)
       --flash-recorder off|on|SECONDS[,HEIGHT]
                         the flash recorder (F8, docs/port/README.md "Debugging"): the last SECONDS of
                         presented pictures (2 for on, the default) kept on the GPU at HEIGHT rows (540),
                         written out as PNGs by the key; Vulkan presenter only
       --fpu-round zero|nearest
                         how the game's float maths rounds: as the EE's FPU (zero, the default:
                         toward zero, no denormals) or as the PC's IEEE floats (nearest), to compare
                         (port/include/port/platform.h)
   With a window and no pad script, settings.json (port/overlay/settings.c, the overlay's F1) fills in
   what the command line does not say. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/automap.h"
#include "port/weapon_sets.h"
#include "port/audio.h"
#include "port/debug.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/settings.h"
#include "port/savestate.h"
#include "port/quicksave.h"
#include "port/teleport.h"
#include "port/cheats.h"
#include "port/stats.h"

static int parse_cheats(const char *list);
#include "port/sanitize.h"

#ifdef _WIN32
#include <windows.h>
/* A crash on Windows says where it was, which `llvm-addr2line -f -e kf4.exe ADDRESS` names (the
   program is linked at a fixed address) */
static LONG WINAPI crash_report(EXCEPTION_POINTERS *e) {
    fprintf(stderr, "[port] crashed: exception 0x%08lx at %p\n", (unsigned long)e->ExceptionRecord->ExceptionCode,
            e->ExceptionRecord->ExceptionAddress);
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, "[port] (a %s of %p)\n", e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                (void *)e->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#if PORT_ASAN
/* ASan's defaults for this program (ASAN_OPTIONS overrides them): no fake stacks, which would keep
   locals outside the EE threads' stacks, where a save state does not see them; no leak check at exit */
const char *__asan_default_options(void) {
    return "detect_stack_use_after_return=0:detect_leaks=0";
}
#endif

PortOptions port_options;
PortStats port_stats;

static FILE *log_file;

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s --disc <image.iso|folder> [--fast] [--frames N] [--input FILE] [--log FILE|-]\n"
            "          [--clock YYYY-MM-DDTHH:MM:SS] [--headless] [--no-gs] [--ps2-scanout] [--host-dir DIR]\n"
            "          [--dump-frames N,N,...] [--dump-dir DIR] [--stats]\n"
            "          [--audio-dump FILE] [--no-audio] [--controls ps2|verdite] [--memcard DIR|none]\n"
            "          [--record-input FILE] [--state-save-at VBLANK[:SLOT|:FILE],...]\n"
            "          [--state-load [VBLANK:]SLOT|FILE] [--quick-save-at VBLANK[:SLOT|:FILE],...]\n"
            "          [--quick-load [VBLANK:]SLOT|FILE,...] [--widescreen] [--widescreen-hud-centred]\n"
            "          [--gs-capture-at VBLANK[:N],...] [--flash-recorder off|on|SECONDS[,HEIGHT]] [--myus-necklace] [--separate-run] [--extended-stats] [--automap] [--automap-cheats] [--weapon-sets] [--show-fps] [--invincible] [--cheats LIST] [--fpu-round zero|nearest]\n"
            "          [--teleport [VBLANK:]MAP[:PLACE]] [--renderer native|gs|both|modern] [--renderer-shown gs|native|heatmap]\n"
            "          [--presenter vulkan|sdl] [--native-scale auto|1-4] [--native-widescreen] [--no-native-widescreen]\n"
            "          [--native-msaa 1|2|4|8] [--flicker-filter] [--scene-scale 1-4] [--scene-msaa 1|2|4|8]\n"
            "          [--upscaler off|bilinear|fsr3|dlss]\n"
            "          [--native-lod scale|ps2|full] [--native-aniso 1|2|4|8|16]\n"
            "          [--native-cutout-aa ssaa|off] [--native-2d smooth|ps2] [--native-colour exact|high]\n"
            "          [--native-layer-z tolerant|ps2] [--native-shine whole|exact]\n"
            "          [--native-wave-uvs ps2|gpu|cpu|late] [--sky modern|ps2] [--test-dma-wait]\n"
            "          [--vu1 hle|interp|check] [--frame-rate 30|60|120|144|240|monitor]\n"
            "          [--draw-distance 1|1.5|2|4|max] [--draw-distance-sky ps2|push]\n",
            program);
    exit(2);
}

static void parse_clock(const char *text) {
    int *f = port_options.clock_fields;

    if (sscanf(text, "%d-%d-%dT%d:%d:%d", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]) != 6 || f[0] < 2000 ||
        f[0] > 2099 || f[1] < 1 || f[1] > 12 || f[2] < 1 || f[2] > 31 || f[3] > 23 || f[4] > 59 || f[5] > 59) {
        fprintf(stderr, "--clock wants YYYY-MM-DDTHH:MM:SS (2000 to 2099), not %s\n", text);
        exit(2);
    }
    port_options.clock_fixed = 1;
}

static const char *gs_capture_at; /* --gs-capture-at (port/debug/gs_capture.c) */

/* --flash-recorder off|on|SECONDS[,HEIGHT] */
static int parse_flash_recorder(const char *value) {
    char *end;
    double seconds;

    if (strcmp(value, "off") == 0) {
        port_options.flash_seconds = 0;
        return 1;
    }
    if (strcmp(value, "on") == 0) {
        port_options.flash_seconds = 2;
        return 1;
    }
    seconds = strtod(value, &end);
    if (end == value || !(seconds >= 0 && seconds <= 30)) {
        return 0;
    }
    port_options.flash_seconds = (float)seconds;
    if (*end == ',') {
        long height = strtol(end + 1, &end, 10);

        if (height < 90 || height > 2160) {
            return 0;
        }
        port_options.flash_height = (int)height;
    }
    return *end == 0;
}

int port_native_draw_parse(const char *value); /* (below) */

static void parse_options(int argc, char **argv) {
    int i;

    port_options.disc = getenv("KF4_DISC");
    port_options.deinterlace = port_options.progressive = port_options.anti_blur = port_options.adapt_width = 1;
    port_options.whole_frame = 1;
    port_options.seamless_scroll = 1;
    port_options.native_scale = 0; /* auto: port_gs_native_scale() */
    port_options.frame_rate = 30;
    port_options.flash_seconds = 2; /* the flash recorder (F8): on */
    port_options.flash_height = 540;
    port_options.present_scaling = -1;
    port_options.present_sharpen = -1;
    port_options.native_msaa = 1;
    port_options.native_lod = 1; /* GSN_LOD_SCALE */
    port_options.native_aniso = 1;
    port_options.native_cutout_aa = 1; /* GSN_CUTOUT_AA_SSAA */
    port_options.native_2d = 1;        /* GSN_2D_SMOOTH */
    port_options.native_layer_z = 1; /* GSN_LAYER_Z_TOLERANT */
    port_options.native_wave_uvs = PORT_WAVE_UVS_PS2;
    port_options.native_sky = PORT_SKY_MODERN;
    port_options.renderer = PORT_RENDERER_NATIVE; /* stage 4 (docs/port/STEP3.md) */
    port_options.presenter = PORT_PRESENTER_VULKAN;
    port_options.vu1 = PORT_VU1_HLE; /* the same results as the interpreter, to the bit, in a fifth of the time */
    for (i = 1; i < argc; i++) {
        const char *option = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;

        if (strcmp(option, "--no-audio") == 0) {
            port_options.no_audio = 1;
            continue;
        }
        if (strcmp(option, "--fast") == 0) {
            port_options.fast = 1;
            continue;
        }
        if (strcmp(option, "--headless") == 0) {
            port_options.headless = 1;
            continue;
        }
        if (strcmp(option, "--no-gs") == 0) {
            port_options.no_gs = 1;
            continue;
        }
        if (strcmp(option, "--ps2-scanout") == 0) {
            port_options.deinterlace = port_options.progressive = port_options.anti_blur = port_options.adapt_width = 0;
            port_options.whole_frame = 0;
            port_options.seamless_scroll = 0;
            port_options.flicker_filter = port_options.flicker_filter_forced = 1;
            continue;
        }
        if (strcmp(option, "--flicker-filter") == 0) {
            port_options.flicker_filter = port_options.flicker_filter_forced = 1;
            continue;
        }
        if (strcmp(option, "--no-deinterlace") == 0) {
            port_options.deinterlace = 0;
            continue;
        }
        if (strcmp(option, "--no-progressive") == 0) {
            port_options.progressive = 0;
            continue;
        }
        if (strcmp(option, "--no-anti-blur") == 0) {
            port_options.anti_blur = 0;
            continue;
        }
        if (strcmp(option, "--no-adapt-width") == 0) {
            port_options.adapt_width = 0;
            continue;
        }
        if (strcmp(option, "--no-whole-frame") == 0) {
            port_options.whole_frame = 0;
            continue;
        }
        if (strcmp(option, "--no-seamless-scroll") == 0) {
            port_options.seamless_scroll = 0;
            continue;
        }
        if (strcmp(option, "--widescreen") == 0 || strcmp(option, "--widescreen-hud-centred") == 0) {
            port_options.widescreen = port_options.widescreen_forced = 1;
            if (strcmp(option, "--widescreen-hud-centred") == 0) {
                port_options.widescreen_hud_centred = 1;
            }
            continue;
        }
        if (strcmp(option, "--native-widescreen") == 0 || strcmp(option, "--no-native-widescreen") == 0) {
            port_options.native_widescreen = strcmp(option, "--native-widescreen") == 0;
            port_options.native_widescreen_forced = 1;
            continue;
        }
        if (strcmp(option, "--invincible") == 0) {
            port_options.invincible = 1;
            continue;
        }
        if (strcmp(option, "--test-dma-wait") == 0) {
            port_options.test_dma_wait = 1;
            continue;
        }
        if (strcmp(option, "--myus-necklace") == 0) {
            port_options.myus_necklace = port_options.myus_necklace_forced = 1;
            continue;
        }
        if (port_automap_option(argc, argv, &i)) { /* the automap mod (port/platform/automap.c) */
            continue;
        }
        if (port_weapon_sets_option(argc, argv, &i)) { /* the weapon sets mod (port/platform/weapon_sets.c) */
            continue;
        }
        if (strcmp(option, "--separate-run") == 0) {
            port_options.separate_run = port_options.separate_run_forced = 1;
            continue;
        }
        if (strcmp(option, "--extended-stats") == 0) { /* the extended stats mod (port/include/port/stats.h) */
            port_stats_options.on = port_stats_options.forced = 1;
            continue;
        }
        if (strcmp(option, "--show-fps") == 0) {
            port_options.show_fps = port_options.show_fps_forced = 1;
            continue;
        }
        if (strcmp(option, "--stats") == 0) {
            port_options.stats = 1;
            continue;
        }
        if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0 || value == NULL) {
            usage(argv[0]);
        }
        i++;
        if (strcmp(option, "--disc") == 0) {
            port_options.disc = value;
        } else if (strcmp(option, "--renderer") == 0 || strcmp(option, "--renderer-shown") == 0) {
            int both = strcmp(option, "--renderer") == 0;
            int heatmap = !both && strcmp(value, "heatmap") == 0;
            int which = heatmap                              ? PORT_RENDERER_NATIVE
                        : strcmp(value, "gs") == 0           ? PORT_RENDERER_GS
                        : strcmp(value, "native") == 0       ? PORT_RENDERER_NATIVE
                        : both && strcmp(value, "modern") == 0 ? PORT_RENDERER_NATIVE
                        : both && strcmp(value, "both") == 0 ? PORT_RENDERER_BOTH
                                                             : -1;
            if (which < 0) {
                fprintf(stderr, "%s wants gs, native%s, not %s\n", option, both ? ", both or modern" : " or heatmap", value);
                exit(2);
            }
            if (both) {
                port_options.renderer = which;
                port_options.renderer_forced = 1;
                port_options.renderer_modern = strcmp(value, "modern") == 0;
            } else {
                port_options.renderer_shown = which;
                port_options.renderer_heatmap = heatmap;
            }
        } else if (strcmp(option, "--presenter") == 0) {
            if (strcmp(value, "sdl") == 0) {
                port_options.presenter = PORT_PRESENTER_SDL;
            } else if (strcmp(value, "vulkan") == 0) {
                port_options.presenter = PORT_PRESENTER_VULKAN;
            } else {
                fprintf(stderr, "--presenter wants sdl or vulkan, not %s\n", value);
                exit(2);
            }
            port_options.presenter_forced = 1;
        } else if (strcmp(option, "--native-scale") == 0) {
            port_options.native_scale = strcmp(value, "auto") == 0 ? 0 : atoi(value);
            if ((port_options.native_scale < 1 && strcmp(value, "auto") != 0) || port_options.native_scale > 4) {
                fprintf(stderr, "--native-scale wants auto or 1 to 4, not %s\n", value);
                exit(2);
            }
            port_options.native_scale_forced = 1;
        } else if (strcmp(option, "--present-scaling") == 0) {
            port_options.present_scaling = strcmp(value, "sharp") == 0    ? PORT_SCALING_SHARP
                                           : strcmp(value, "smooth") == 0 ? PORT_SCALING_SMOOTH
                                           : strcmp(value, "best") == 0   ? PORT_SCALING_BEST
                                                                          : -2;
            if (port_options.present_scaling == -2) {
                fprintf(stderr, "--present-scaling wants sharp, smooth or best, not %s\n", value);
                exit(2);
            }
        } else if (strcmp(option, "--present-sharpen") == 0) {
            port_options.present_sharpen = (float)atof(value);
            if (port_options.present_sharpen < 0 || port_options.present_sharpen > 1) {
                fprintf(stderr, "--present-sharpen wants 0 to 1, not %s\n", value);
                exit(2);
            }
        } else if (strcmp(option, "--dump-present") == 0) {
            if (sscanf(value, "%dx%d", &port_options.dump_present_width, &port_options.dump_present_height) != 2 ||
                port_options.dump_present_width < 16 || port_options.dump_present_height < 16 ||
                port_options.dump_present_width > 8192 || port_options.dump_present_height > 8192) {
                fprintf(stderr, "--dump-present wants WIDTHxHEIGHT, not %s\n", value);
                exit(2);
            }
        } else if (strcmp(option, "--frame-rate") == 0) {
            port_options.frame_rate = strcmp(value, "monitor") == 0 ? PORT_FRAME_RATE_MONITOR : atoi(value);
            if (port_options.frame_rate != PORT_FRAME_RATE_MONITOR &&
                (port_options.frame_rate < 30 || port_options.frame_rate > 500)) {
                fprintf(stderr, "--frame-rate wants 30 to 500 (30, 60, 120, 144, 240 ...) or monitor, not %s\n", value);
                exit(2);
            }
            port_options.frame_rate_forced = 1;
        } else if (strcmp(option, "--draw-distance") == 0) {
            port_options.draw_distance = port_draw_distance_parse(value);
            if (port_options.draw_distance < 0) {
                fprintf(stderr, "--draw-distance wants 1, 1.5, 2, 4 or max, not %s\n", value);
                exit(2);
            }
            port_options.draw_distance_forced = 1;
        } else if (strcmp(option, "--draw-distance-sky") == 0) {
            if (strcmp(value, "push") == 0) {
                port_options.draw_distance_sky = 1;
            } else if (strcmp(value, "ps2") == 0) {
                port_options.draw_distance_sky = 0;
            } else {
                fprintf(stderr, "--draw-distance-sky wants ps2 or push, not %s\n", value);
                exit(2);
            }
            port_options.draw_distance_sky_forced = 1;
        } else if (strcmp(option, "--scene-scale") == 0) {
            port_options.scene_scale = atoi(value);
            if (port_options.scene_scale < 1 || port_options.scene_scale > 4) {
                fprintf(stderr, "--scene-scale wants 1 to 4, not %s\n", value);
                exit(2);
            }
        } else if (strcmp(option, "--upscaler") == 0) {
            if (strcmp(value, "off") == 0) {
                port_options.upscaler = 0;
            } else if (strcmp(value, "bilinear") == 0) {
                port_options.upscaler = 1;
            } else if (strcmp(value, "fsr3") == 0) {
                port_options.upscaler = 2;
            } else if (strcmp(value, "dlss") == 0) {
                port_options.upscaler = 3;
            } else {
                fprintf(stderr, "--upscaler wants off, bilinear, fsr3 or dlss, not %s\n", value);
                exit(2);
            }
        } else if (strcmp(option, "--scene-msaa") == 0) {
            port_options.scene_msaa = atoi(value);
            if (port_options.scene_msaa != 1 && port_options.scene_msaa != 2 && port_options.scene_msaa != 4 &&
                port_options.scene_msaa != 8) {
                fprintf(stderr, "--scene-msaa wants 1, 2, 4 or 8, not %s\n", value);
                exit(2);
            }
        } else if (strcmp(option, "--native-msaa") == 0) {
            port_options.native_msaa = atoi(value);
            if (port_options.native_msaa != 1 && port_options.native_msaa != 2 && port_options.native_msaa != 4 &&
                port_options.native_msaa != 8) {
                fprintf(stderr, "--native-msaa wants 1, 2, 4 or 8, not %s\n", value);
                exit(2);
            }
            port_options.native_msaa_forced = 1;
        } else if (strcmp(option, "--native-lod") == 0) {
            if (strcmp(value, "ps2") == 0) {
                port_options.native_lod = 0;
            } else if (strcmp(value, "scale") == 0) {
                port_options.native_lod = 1;
            } else if (strcmp(value, "full") == 0) {
                port_options.native_lod = 2;
            } else {
                fprintf(stderr, "--native-lod wants scale, ps2 or full, not %s\n", value);
                exit(2);
            }
            port_options.native_lod_forced = 1;
        } else if (strcmp(option, "--native-aniso") == 0) {
            port_options.native_aniso = atoi(value);
            if (port_options.native_aniso != 1 && port_options.native_aniso != 2 && port_options.native_aniso != 4 &&
                port_options.native_aniso != 8 && port_options.native_aniso != 16) {
                fprintf(stderr, "--native-aniso wants 1, 2, 4, 8 or 16, not %s\n", value);
                exit(2);
            }
            port_options.native_aniso_forced = 1;
        } else if (strcmp(option, "--native-cutout-aa") == 0) {
            if (strcmp(value, "off") == 0) {
                port_options.native_cutout_aa = 0;
            } else if (strcmp(value, "ssaa") == 0) {
                port_options.native_cutout_aa = 1;
            } else {
                fprintf(stderr, "--native-cutout-aa wants ssaa or off, not %s\n", value);
                exit(2);
            }
            port_options.native_cutout_aa_forced = 1;
        } else if (strcmp(option, "--native-2d") == 0) {
            if (strcmp(value, "ps2") == 0) {
                port_options.native_2d = 0;
            } else if (strcmp(value, "smooth") == 0) {
                port_options.native_2d = 1;
            } else {
                fprintf(stderr, "--native-2d wants smooth or ps2, not %s\n", value);
                exit(2);
            }
            port_options.native_2d_forced = 1;
        } else if (strcmp(option, "--native-colour") == 0) {
            if (strcmp(value, "exact") == 0) {
                port_options.native_colour = 0;
            } else if (strcmp(value, "high") == 0) {
                port_options.native_colour = 1;
            } else {
                fprintf(stderr, "--native-colour wants exact or high, not %s\n", value);
                exit(2);
            }
            port_options.native_colour_forced = 1;
        } else if (strcmp(option, "--native-draw") == 0) {
            int bits = port_native_draw_parse(value);

            if (bits < 0) {
                fprintf(stderr, "--native-draw wants off, on or a list of map, models, skinned (entities), arm, sky, effects, "
                                "shadows, reflection, water, passes, items, objects, picked, gold; not %s\n", value);
                exit(2);
            }
            port_options.native_draw = bits;
            port_options.native_draw_forced = 1;
        } else if (strcmp(option, "--native-draw-compare") == 0) {
            if (strcmp(value, "native") == 0) {
                port_options.native_draw_compare = PORT_ND_COMPARE_NATIVE;
            } else if (strcmp(value, "ps2") == 0) {
                port_options.native_draw_compare = PORT_ND_COMPARE_PS2;
            } else if (strcmp(value, "off") == 0) {
                port_options.native_draw_compare = 0;
            } else {
                fprintf(stderr, "--native-draw-compare wants native, ps2 or off, not %s\n", value);
                exit(2);
            }
        } else if (strcmp(option, "--native-wave-uvs") == 0) {
            if (strcmp(value, "ps2") == 0) {
                port_options.native_wave_uvs = PORT_WAVE_UVS_PS2;
            } else if (strcmp(value, "gpu") == 0) {
                port_options.native_wave_uvs = PORT_WAVE_UVS_GPU;
            } else if (strcmp(value, "cpu") == 0) {
                port_options.native_wave_uvs = PORT_WAVE_UVS_CPU;
            } else if (strcmp(value, "late") == 0) {
                port_options.native_wave_uvs = PORT_WAVE_UVS_LATE;
            } else {
                fprintf(stderr, "--native-wave-uvs wants ps2, gpu, cpu or late, not %s\n", value);
                exit(2);
            }
        } else if (strcmp(option, "--sky") == 0) {
            if (strcmp(value, "ps2") == 0) {
                port_options.native_sky = PORT_SKY_PS2;
            } else if (strcmp(value, "modern") == 0) {
                port_options.native_sky = PORT_SKY_MODERN;
            } else {
                fprintf(stderr, "--sky wants modern or ps2, not %s\n", value);
                exit(2);
            }
            port_options.native_sky_forced = 1;
        } else if (strcmp(option, "--native-layer-z") == 0) {
            if (strcmp(value, "ps2") == 0) {
                port_options.native_layer_z = 0;
            } else if (strcmp(value, "tolerant") == 0) {
                port_options.native_layer_z = 1;
            } else {
                fprintf(stderr, "--native-layer-z wants tolerant or ps2, not %s\n", value);
                exit(2);
            }
            port_options.native_layer_z_forced = 1;
        } else if (strcmp(option, "--native-shine") == 0) {
            if (strcmp(value, "whole") == 0) {
                port_options.native_shine_exact = 0;
            } else if (strcmp(value, "exact") == 0) {
                port_options.native_shine_exact = 1;
            } else {
                fprintf(stderr, "--native-shine wants whole or exact, not %s\n", value);
                exit(2);
            }
            port_options.native_shine_exact_forced = 1;
        } else if (strcmp(option, "--vu1") == 0) {
            if (strcmp(value, "interp") == 0) {
                port_options.vu1 = PORT_VU1_INTERP;
            } else if (strcmp(value, "hle") == 0) {
                port_options.vu1 = PORT_VU1_HLE;
            } else if (strcmp(value, "check") == 0) {
                port_options.vu1 = PORT_VU1_CHECK;
            } else {
                fprintf(stderr, "--vu1 wants interp, hle or check, not %s\n", value);
                exit(2);
            }
        } else if (strcmp(option, "--frames") == 0) {
            port_options.frames = strtoll(value, NULL, 0);
        } else if (strcmp(option, "--input") == 0) {
            port_options.input = value;
        } else if (strcmp(option, "--log") == 0) {
            port_options.log = value;
        } else if (strcmp(option, "--clock") == 0) {
            parse_clock(value);
        } else if (strcmp(option, "--host-dir") == 0) {
            port_options.host_dir = value;
        } else if (strcmp(option, "--dump-frames") == 0) {
            port_options.dump_frames = value;
        } else if (strcmp(option, "--audio-dump") == 0) {
            port_options.audio_dump = value;
        } else if (strcmp(option, "--record-input") == 0) {
            port_options.record_input = value;
        } else if (strcmp(option, "--memcard") == 0) {
            port_options.memcard = value;
        } else if (strcmp(option, "--controls") == 0) {
            port_options.controls = value;
        } else if (strcmp(option, "--dump-dir") == 0) {
            port_options.dump_dir = value;
        } else if (strcmp(option, "--state-save-at") == 0) {
            port_options.state_save_at = value;
        } else if (strcmp(option, "--state-load") == 0) {
            port_options.state_load = value;
        } else if (strcmp(option, "--quick-save-at") == 0) {
            port_options.quick_save_at = value;
        } else if (strcmp(option, "--quick-load") == 0) {
            port_options.quick_load = value;
        } else if (strcmp(option, "--cheats") == 0) {
            if (!parse_cheats(value)) {
                usage(argv[0]);
            }
        } else if (strcmp(option, "--teleport") == 0) {
            port_options.teleport = value;
        } else if (strcmp(option, "--gs-capture-at") == 0) {
            gs_capture_at = value;
        } else if (strcmp(option, "--flash-recorder") == 0) {
            if (!parse_flash_recorder(value)) {
                usage(argv[0]);
            }
        } else if (strcmp(option, "--fpu-round") == 0) {
            if (strcmp(value, "zero") != 0 && strcmp(value, "nearest") != 0) {
                usage(argv[0]);
            }
            port_fpu_round_nearest = strcmp(value, "nearest") == 0;
        } else {
            usage(argv[0]);
        }
    }
    if (port_options.renderer_modern && port_options.native_draw == 0) {
        port_options.native_draw = PORT_ND_ACCEPTED; /* (the scene's kinds: every one, P8 T1) */
        port_options.native_draw_forced = 1; /* (settings.json's "off" must not empty the scene) */
    }
    if (port_options.disc == NULL) {
        fprintf(stderr, "kf4 needs your own King's Field IV disc: --disc <image.iso or folder> (or KF4_DISC)\n");
        usage(argv[0]);
    }
}

static const char *const draw_distance_names[PORT_DRAW_DISTANCE_COUNT] = {"1", "1.5", "2", "4", "max"};
static const float draw_distance_factors[PORT_DRAW_DISTANCE_COUNT] = {1.0f, 1.5f, 2.0f, 4.0f,
                                                                      PORT_DRAW_DISTANCE_MAX_FOG};

int port_draw_distance_parse(const char *name) {
    int i;

    for (i = 0; i < PORT_DRAW_DISTANCE_COUNT; i++) {
        if (strcmp(name, draw_distance_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *port_draw_distance_name(int level) {
    return level >= 0 && level < PORT_DRAW_DISTANCE_COUNT ? draw_distance_names[level] : draw_distance_names[0];
}

float port_draw_distance(void) {
    int level = port_options.draw_distance;

    return level > 0 && level < PORT_DRAW_DISTANCE_COUNT ? draw_distance_factors[level] : 1.0f;
}

int port_draw_distance_sky(void) {
    return port_options.draw_distance_sky != 0;
}

float port_draw_distance_far(void) {
    float factor = port_draw_distance();

    return factor > 16.0f ? 16.0f : factor;
}

int port_myus_necklace(void) {
    return port_options.myus_necklace != 0;
}

/* --cheats spells,equipment,items,magic or all: 0 for a word it does not know */
static int parse_cheats(const char *list) {
    static const struct {
        const char *name;
        int bits;
    } names[] = {{"spells", PORT_CHEAT_SPELLS}, {"spells1", PORT_CHEAT_SPELLS | 0x100}, {"spells2", PORT_CHEAT_SPELLS | 0x200},
                 {"spells3", PORT_CHEAT_SPELLS | 0x300}, {"equipment", PORT_CHEAT_EQUIPMENT}, {"items", PORT_CHEAT_ITEMS},
                 {"magic", PORT_CHEAT_MAGIC},
                 {"all", PORT_CHEAT_SPELLS | PORT_CHEAT_EQUIPMENT | PORT_CHEAT_ITEMS | PORT_CHEAT_MAGIC}};
    const char *at = list;

    while (*at != 0) {
        size_t length = strcspn(at, ",");
        size_t i;
        int found = 0;

        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (strlen(names[i].name) == length && strncmp(at, names[i].name, length) == 0) {
                if (names[i].bits >> 8) {
                    port_cheat_set_spell_level(names[i].bits >> 8);
                }
                port_cheat_request(names[i].bits & 0xFF);
                found = 1;
            }
        }
        if (!found) {
            return 0;
        }
        at += length;
        if (*at == ',') {
            at++;
        }
    }
    return 1;
}

int port_invincible(void) {
    return port_options.invincible != 0;
}

int port_native_draw(int kind) {
    return (port_options.native_draw & kind) != 0;
}

int port_native_draw_compare(void) {
    return port_options.native_draw ? port_options.native_draw_compare : 0;
}

int port_native_wave_uvs(void) {
    return port_options.native_wave_uvs;
}

int port_native_sky(void) {
    return port_options.native_sky;
}

int port_test_dma_wait(void) {
    return port_options.test_dma_wait;
}

/* --native-draw's value: off, on, or a comma-separated list of kinds; -1 if it is none of those */
int port_native_draw_parse(const char *value) {
    static const struct {
        const char *name;
        int bits;
    } kinds[] = {{"map", PORT_ND_MAP}, {"models", PORT_ND_MODELS}, {"skinned", PORT_ND_SKINNED}, {"entities", PORT_ND_SKINNED}, {"arm", PORT_ND_ARM},
                 {"sky", PORT_ND_SKY}, {"effects", PORT_ND_EFFECTS}, {"shadows", PORT_ND_SHADOWS},
                 {"reflection", PORT_ND_REFLECTION}, {"water", PORT_ND_WATER}, {"passes", PORT_ND_PASSES},
                 {"items", PORT_ND_ITEMS},
                 {"objects", PORT_ND_OBJECTS}, {"picked", PORT_ND_PICKED}, {"gold", PORT_ND_GOLD},
                 {"on", PORT_ND_ACCEPTED}}; /* ("on,passes": the accepted kinds and more) */
    const char *at = value;
    int bits = 0;

    if (strcmp(value, "off") == 0) {
        return 0;
    }
    if (strcmp(value, "on") == 0) {
        return PORT_ND_ACCEPTED;
    }
    while (*at) {
        size_t length = strcspn(at, ",");
        size_t i;
        int found = 0;

        for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
            if (strlen(kinds[i].name) == length && strncmp(at, kinds[i].name, length) == 0) {
                bits |= kinds[i].bits;
                found = 1;
            }
        }
        if (!found) {
            return -1;
        }
        at += length;
        if (*at == ',') {
            at++;
        }
    }
    return bits;
}

/* ---- The log ---------------------------------------------------------------------------------- */
int port_log_enabled(void) {
    return log_file != NULL;
}

/* (both format in the host's float mode: they are called from the game's threads) */
void port_log(const char *fmt, ...) {
    va_list args;
    uint32_t mode;

    if (log_file == NULL) {
        return;
    }
    mode = port_host_float_begin();
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);
    fputc('\n', log_file);
    port_host_float_end(mode);
}

void port_event(const char *fmt, ...) {
    va_list args;
    uint32_t mode = port_host_float_begin();

    va_start(args, fmt);
    port_debug_note_event(port_vblank_count(), fmt, args); /* F10's last events (port/debug/debug.c) */
    va_end(args);
    if (log_file != NULL) {
        fprintf(log_file, "%llu event: ", (unsigned long long)port_vblank_count());
        va_start(args, fmt);
        vfprintf(log_file, fmt, args);
        va_end(args);
        fputc('\n', log_file);
    }
    port_host_float_end(mode);
}

static void open_log(void) {
    if (port_options.log == NULL) {
        return;
    }
    if (strcmp(port_options.log, "-") == 0) {
        log_file = stderr;
        return;
    }
    log_file = fopen(port_options.log, "w");
    if (log_file == NULL) {
        fprintf(stderr, "cannot write the log %s\n", port_options.log);
        exit(2);
    }
    setvbuf(log_file, NULL, _IOLBF, 0); /* whole lines, so a fatal error leaves the log up to it */
}

static void close_log(void) {
    if (log_file != NULL && log_file != stderr) {
        fclose(log_file);
    }
    log_file = NULL;
}

/* ---- --stats ---------------------------------------------------------------------------------- */
static double start_seconds;

static void drained(void *unused) {
    (void)unused;
}

static void print_stats(void) {
    uint32_t mode = port_host_float_begin(); /* (at exit, which the game's thread may call) */
    double per_second = port_ticks_per_second();
    double vu1 = (double)port_stats.vu1_ticks / per_second;

    /* the controller's thread finishes what the game started first, so the counts are the same
       every run */
    port_dmac_call(drained, NULL);
    fprintf(stderr, "[stats] wall %.3f s\n", port_wall_seconds() - start_seconds);
    port_gs_print_stats();
    if (port_frame_interp()) {
        port_interp_print_stats();
    }
    if (port_whole_frame()) {
        fprintf(stderr, "[stats] whole frames: %llu fields drawn again for the other field\n",
                (unsigned long long)port_field_frames_replays());
    }
    port_video_print_stats();
    if (port_options.renderer == PORT_RENDERER_BOTH) {
        const PortRendererDiff *d = &port_renderer_diff;

        fprintf(stderr, "[stats] renderers: %llu pictures compared, %llu differ (a channel by more than 1); "
                        "the largest difference %u at vblank %llu\n",
                (unsigned long long)d->frames, (unsigned long long)d->frames_differ, d->worst_max,
                (unsigned long long)d->worst_vblank);
    }
    fprintf(stderr, "[stats] VU1 %.3f s: %llu runs, %llu instructions (%.2f ns each)\n", vu1,
            (unsigned long long)port_stats.vu1_runs, (unsigned long long)port_stats.vu1_insns,
            port_stats.vu1_insns ? vu1 * 1e9 / (double)port_stats.vu1_insns : 0.0);
    if (port_options.vu1 == PORT_VU1_HLE) {
        fprintf(stderr, "[stats] VU1 HLE: %llu of the runs, %.3f s of the time\n",
                (unsigned long long)port_stats.vu1_hle_runs, (double)port_stats.vu1_hle_ticks / per_second);
    } else if (port_options.vu1 == PORT_VU1_CHECK) {
        fprintf(stderr, "[stats] VU1 check: %llu runs compared, %llu differ\n",
                (unsigned long long)port_vu1_check_runs, (unsigned long long)port_vu1_check_differ);
    }
    fprintf(stderr, "[stats] GIF hand-off %.3f s, vsync %.3f s\n", (double)port_stats.gif_ticks / per_second,
            (double)port_stats.vsync_ticks / per_second);
    fprintf(stderr, "[stats] VU1 cycles %llu, PATH1 hash %016llx\n",
            (unsigned long long)port_stats.vu1_cycles, (unsigned long long)port_stats.path1_hash);
    port_host_float_end(mode);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    parse_options(argc, argv);
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_report);
#endif
    port_int_divide_init(); /* INT_MIN / -1 as the EE gives it */
    port_settings_load(); /* settings.json, where the command line left an option alone */
    start_seconds = port_wall_seconds();
    open_log();
    atexit(close_log);
    if (port_options.stats) {
        atexit(print_stats);
    }
    port_memory_init();
    port_address_init();
    port_disc_open(port_options.disc);
    port_mc_init();
    port_kernel_init();
    port_dmac_start();
    port_debug_init(gs_capture_at); /* before the GS: a capture asked for makes a --headless run draw */
    port_gs_init();
    port_pad_init();
    port_video_init();
    port_audio_init();
    port_state_init();
    port_quick_init();
    port_teleport_init();
    fprintf(stderr, "[port] game RAM at %p, running the game\n", (void *)ee_ram);
    return port_kernel_run(kf4_game_main);
}

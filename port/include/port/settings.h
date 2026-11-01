/* The player's settings (port/overlay/settings.c): what the overlay (F1) changes, kept in
   settings.json in the user's data folder. Options on the command line override them for that run.
   A run that is headless or plays a pad script (--input) reads no settings.json, so tests stay the
   same whatever the player has chosen. */
#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASPECT_4_3 0     /* the TV's 4:3, whatever the window's shape */
#define ASPECT_STRETCH 1 /* the whole window */

typedef struct {
    /* Display */
    int window_height;     /* the window's height in pixels; 896 by default */
    int window_width;      /* its width in pixels (a monitor's size: 2560 x 1600, 3440 x 1440...), or 0 to follow
                              the picture's shape (4:3, or 16:9 with widescreen); 0 by default */
    int fullscreen;        /* borderless, on the whole screen */
    int aspect;            /* ASPECT_ */
    int vsync;             /* wait for the screen's refresh to show each picture */
    int smooth;            /* scale the picture with bilinear filtering (else nearest) */
    int scaling;           /* PORT_SCALING_: sharp (nearest), smooth (bilinear) or best (port_video_scaling) */
    float sharpen;         /* the sharpening after the scaling, 0 (none) to 1 */
    int deinterlace;       /* the ways the PC picture differs from the GS's (port_options) */
    int progressive;
    int anti_blur;
    int adapt_width;
    int whole_frame;
    int seamless_scroll;   /* the scrolling textures without a seam (port_options.seamless_scroll; on) */
    int flicker_filter;    /* the game's flicker filter, as on the PS2 (port_options.flicker_filter; off) */
    int widescreen;        /* the opt-in 16:9 picture (port_options.widescreen) */
    int widescreen_hud_centred; /* with it, the HUD in the 4:3 middle instead of the 16:9 corner */
    int frame_rate;        /* 30 (the PS2's), or the opt-in higher rates, interpolated: pictures a second, or
                              PORT_FRAME_RATE_MONITOR or _UNLOCKED (port_options.frame_rate) */
    int frame_rate_cap;    /* unlocked no faster than this many a second; 0: no cap (port_options.frame_rate_cap) */
    int tearing;           /* unlocked's swapchain immediate, not mailbox (port_options.tearing; off) */
    int draw_distance;     /* the opt-in longer view: PORT_DRAW_DISTANCE_ (port_options.draw_distance) */
    int draw_distance_sky; /* with it, the sky's fog pushed out too (port_options.draw_distance_sky) */
    int renderer;          /* who draws, from the next start: PORT_RENDERER_ (port_options.renderer) */
    int native_scale;      /* the native GS's resolution, 1 to 4 times the PS2's (port_options.native_scale) */
    int native_widescreen; /* with widescreen, the native GS's picture drawn wider (port_options.native_widescreen) */
    int native_msaa;       /* the native GS's samples a pixel for the 3D: 1, 2, 4, 8 (port_options.native_msaa) */
    int native_lod;        /* the native GS's mipmap levels: 0 ps2, 1 scale, 2 full (port_options.native_lod) */
    int native_aniso;      /* the native GS's anisotropic filtering: 1 off, 2 to 16 (port_options.native_aniso) */
    int native_cutout_aa;  /* with MSAA, the native GS's cut-outs: 0 off, 1 ssaa (port_options.native_cutout_aa) */
    int native_2d;         /* at a larger scale, the native GS's 2D: 0 ps2, 1 smooth (port_options.native_2d) */
    int native_colour;     /* the native GS's colour precision: 0 exact, 1 high (port_options.native_colour) */
    int native_layer_z;    /* the native GS's Z: 0 ps2, 1 tolerant (port_options.native_layer_z) */
    int native_shine_exact; /* modern's shines blended in the shader: 0 whole, 1 exact (port_options.native_shine_exact) */
    int native_draw;       /* native draw, the whole path: 0 off (the PS2 path), 1 every accepted kind
                              (port_options.native_draw = PORT_ND_ACCEPTED; docs/port/NATIVE_DRAW.md) */
    int native_sky;        /* the native sky: PORT_SKY_PS2 or PORT_SKY_MODERN (port_options.native_sky) */
    int presenter;         /* how the picture reaches the window, from the next start: PORT_PRESENTER_ */
    /* Audio */
    int volume;            /* 0 to 100 */
    int mute;
    /* Controls (the bindings are port/sdk/controls.c's) */
    char layout[16];       /* "ps2" or "verdite" */
    /* Mods */
    int myus_necklace;     /* the effect of Myu's Necklace, always (port_options.myus_necklace) */
    int separate_run;      /* a run key apart from the action button (port_options.separate_run) */
    /* The overlay */
    int pause_when_open;
    int show_fps;
    float ui_scale;        /* the overlay's size, 0.75 to 2 */
    int save_slot;         /* the slot the Save states page has chosen */
    int quick_slot;        /* the slot of the quick save and quick load keys (port/include/port/quicksave.h) */
} PortSettings;
extern PortSettings port_settings;

/* (main) Reads settings.json, unless the run is headless or plays a pad script, and puts its values
   into port_options where the command line left them alone. Writes it back at exit if changed. */
void port_settings_load(void);
/* 1 when settings.json was read (a run with a window and no pad script) */
int port_settings_active(void);
/* Something changed: written a second after the last change, and at exit */
void port_settings_changed(void);
/* (once a vblank, the game's thread) writes settings.json if it changed a while ago */
void port_settings_poll(void);
void port_settings_save(void);
/* The user's data folder: KF4_DATA_DIR, else %APPDATA%\holyland on Windows, $XDG_CONFIG_HOME/holyland
   (or ~/.config/holyland) elsewhere. Made when first written to. */
const char *port_data_dir(void);
/* settings.json's path */
const char *port_settings_path(void);
/* Puts the audio settings into the sound output (port_audio_set_volume) */
void port_settings_apply_audio(void);
/* Puts the display settings into the window: its size, full screen, vsync (port/platform/video.c) */
void port_video_apply_settings(void);

#ifdef __cplusplus
}
#endif

#endif
